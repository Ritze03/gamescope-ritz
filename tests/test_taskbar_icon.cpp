// Unit tests for the taskbar icon helpers: key hashing, _NET_WM_ICON parsing,
// the .desktop text, and the files EnsureForIcon() writes (into a temp
// XDG_DATA_HOME). See superdoc/features/backend-wayland.md.
#include <catch2/catch_test_macros.hpp>

#include "Utils/TaskbarIcon.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace gamescope::taskbar_icon;

TEST_CASE( "key is stable, prefixed and pixel-dependent", "[taskbar_icon]" )
{
	std::vector<uint32_t> a = { 2, 1, 0xff112233, 0x80445566 };
	std::vector<uint32_t> b = { 2, 1, 0xff112233, 0x80445567 };
	Image ia, ib;
	REQUIRE( LargestImage( a, ia ) );
	REQUIRE( LargestImage( b, ib ) );
	std::string ka = KeyForImage( ia );
	REQUIRE( ka.size() == std::string( "gamescope-ritz-" ).size() + 8 );
	REQUIRE( ka.rfind( "gamescope-ritz-", 0 ) == 0 );
	REQUIRE( ka == KeyForImage( ia ) );
	REQUIRE( ka != KeyForImage( ib ) );
}

TEST_CASE( "LargestImage walks back-to-back images and rejects truncation", "[taskbar_icon]" )
{
	std::vector<uint32_t> d = { 1, 1, 7, 2, 2, 1, 2, 3, 4 };
	Image i;
	REQUIRE( LargestImage( d, i ) );
	REQUIRE( i.uWidth == 2 );
	REQUIRE( i.pPixels[3] == 4 );
	std::vector<uint32_t> bad = { 4, 4, 1 };
	REQUIRE( !LargestImage( bad, i ) );
}

TEST_CASE( "desktop entry text", "[taskbar_icon]" )
{
	std::string t = DesktopEntryText( "k", "A\nB", "/x/k.png" );
	REQUIRE( t.find( "Name=AB\n" ) != std::string::npos );
	REQUIRE( t.find( "Icon=/x/k.png\n" ) != std::string::npos );
	REQUIRE( t.find( "NoDisplay=true\n" ) != std::string::npos );
	REQUIRE( t.find( "StartupWMClass=k\n" ) != std::string::npos );
}

TEST_CASE( "EnsureForIcon and EnsureFallback write files", "[taskbar_icon]" )
{
	std::filesystem::path dir = std::filesystem::path( getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "." ) / "taskbar_icon_test";
	std::filesystem::remove_all( dir );
	setenv( "XDG_DATA_HOME", dir.c_str(), 1 );

	std::vector<uint32_t> d = { 2, 2, 0xff000000, 0xffff0000, 0xff00ff00, 0xff0000ff };
	std::string key = EnsureForIcon( d );
	REQUIRE( !key.empty() );
	REQUIRE( std::filesystem::exists( dir / "applications" / ( key + ".desktop" ) ) );
	REQUIRE( std::filesystem::exists( dir / "icons/hicolor/2x2/apps" / ( key + ".png" ) ) );

	REQUIRE( EnsureFallback() == "gamescope-ritz" );
	REQUIRE( std::filesystem::exists( dir / "applications/gamescope-ritz.desktop" ) );
	REQUIRE( std::filesystem::exists( dir / "icons/hicolor/256x256/apps/gamescope-ritz.png" ) );
	REQUIRE( ProjectIconPixels()[0] == 256 );
	std::filesystem::remove_all( dir );
}
