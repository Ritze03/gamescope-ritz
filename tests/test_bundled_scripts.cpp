// The bundled Lua tree, compiled into the binary (2026-09-09).
//
// WHY THIS TEST EXISTS. scripts/00-gamescope/ is the known-displays database
// DRMBackend.cpp looks a panel up in. It used to reach disk only through an
// install step install.sh PROMPTED for and defaulted to declining, so the
// ordinary install had no database at all and nothing but a log line said so.
// It is compiled in now (src/Script/embed_scripts.py) -- and the failure mode
// that replaces "the user declined the prompt" is "the generator emitted the
// chunks in the wrong order", which is silent in exactly the same way: the
// displays/*.lua files call zero_index() from common/util.lua and
// gamescope.modegen from common/modegen.lua AT LOAD TIME, so a wrong order
// does not fail to compile, it fails to populate.
//
// So this runs the embedded chunks through a real sol::state -- the same
// CScriptManager the compositor uses -- and asserts on what the data
// PROVIDES, not on the absence of an error.
#include <catch2/catch_test_macros.hpp>

#include "Script/Script.h"

#include <string>
#include <vector>

using namespace gamescope;

TEST_CASE( "bundled scripts populate the known-displays database", "[bundled_scripts]" )
{
	{
		CScriptScopedLock script;
		script.Manager().RunBundledScripts();
	}

	SECTION( "every bundled display is registered, with its pretty name" )
	{
		CScriptScopedLock script;
		sol::table tDisplays = script.Manager().Gamescope().Config.KnownDisplays;

		size_t nCount = 0;
		for ( auto iter : tDisplays )
		{
			(void)iter;
			nCount++;
		}
		// The tree ships ten displays today; a smaller number means chunks
		// went missing, which is the whole thing this guards.
		CHECK( nCount == 10 );

		// A spot check on the two the fork is most likely to meet, by key
		// and by a field only that file defines.
		sol::optional<sol::table> otDeck = tDisplays[ "steamdeck_oled_sdc" ];
		REQUIRE( otDeck );
		CHECK( ( *otDeck )[ "pretty_name" ].get<std::string>() == "Steam Deck OLED (SDC)" );

		sol::optional<sol::table> otAlly = tDisplays[ "rogally_lcd" ];
		REQUIRE( otAlly );
		CHECK( ( *otAlly )[ "pretty_name" ].get<std::string>()
			== "ASUS ROG Ally / ROG Ally X LCD" );
	}

	SECTION( "common/ ran before displays/, so a mode generator actually works" )
	{
		// THE ORDER CONTRACT, ASSERTED BY EXERCISE. dynamic_modegen reaches
		// zero_index() (common/util.lua) and gamescope.modegen
		// (common/modegen.lua). If the generator had emitted displays/ first,
		// the table above would still exist -- the field would just be a
		// function that throws the moment it is called. So call it.
		CScriptScopedLock script;
		sol::table tDisplays = script.Manager().Gamescope().Config.KnownDisplays;

		sol::optional<sol::table> otDeck = tDisplays[ "steamdeck_oled_sdc" ];
		REQUIRE( otDeck );
		sol::optional<sol::protected_function> ofnModegen = ( *otDeck )[ "dynamic_modegen" ];
		REQUIRE( ofnModegen );

		// A realistic Steam Deck OLED base mode, so calc_vrefresh() has real
		// numbers rather than a division by zero.
		sol::table tMode = script->create_table();
		tMode[ "hdisplay" ]    = 800;
		tMode[ "vdisplay" ]    = 1280;
		tMode[ "hsync_start" ] = 850;
		tMode[ "hsync_end" ]   = 858;
		tMode[ "htotal" ]      = 890;
		tMode[ "vsync_start" ] = 1360;
		tMode[ "vsync_end" ]   = 1364;
		tMode[ "vtotal" ]      = 1370;
		tMode[ "clock" ]       = 97000;
		tMode[ "vrefresh" ]    = 0;

		sol::protected_function_result res = ( *ofnModegen )( tMode, 90 );
		REQUIRE( res.valid() );

		sol::table tOut = res;
		// adjust_front_porch() rewrites the vertical timings from the
		// per-refresh front-porch table in valve.steamdeck.oled.lua, so the
		// mode that comes back is not the one that went in.
		CHECK( tOut[ "vsync_start" ].get<int>() != 1360 );
		CHECK( tOut[ "vtotal" ].get<int>() > tOut[ "vsync_end" ].get<int>() );
		CHECK( tOut[ "vrefresh" ].get<int>() > 0 );
	}

	SECTION( "matches() answers, which is what the DRM backend calls" )
	{
		CScriptScopedLock script;
		auto oDisplay = script.Manager().Gamescope().Config.LookupDisplay(
			script, "VLV", 0x3003, "", "" );
		REQUIRE( oDisplay );
		CHECK( oDisplay->first == "steamdeck_oled_sdc" );
	}
}
