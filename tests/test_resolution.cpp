// Unit tests for Overlay/ResolutionPresets.h -- the Resolution area's pure
// half: the aspect shapes, their size lists, the classification of a live
// width/height, and the closest-by-height pick a shape switch makes
// (superdoc/features/resolution-and-refresh.md).
//
// Nothing here needs ImGui, a backend or a compositor, which is why the
// maths was split out of PanelDisplay.cpp into a header in the first place
// -- the same split Overlay/CrosshairMath.h already makes.
//
// The last case covers the ONE row's compatibility story: the four
// per-aspect rows removed on 2026-09-06 (requests item 7) never had a config
// key, so an old config carrying their ids has nothing to migrate and must
// simply load.
//
// requests-2026-09-07 items 3 and 4 add two more pure things to pin:
//   - ClassifyAspect() -- the reflection rule PanelDisplay.cpp's
//     CurrentAspect() now delegates to for "no trusted pick" -- must land on
//     Custom for anything that isn't an exact size-list entry or the output's
//     own size, since the Resolution dropdown no longer has a "Custom" entry
//     for a real shape to hide the mismatch behind (item 3).
//   - FormatLiveLine() -- the Live-state row's exact wording (item 4).
#include <catch2/catch_test_macros.hpp>

#include "Overlay/ResolutionPresets.h"
#include "Config/ConfigManager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace gamescope::resolution;
using namespace gamescope::config;

namespace
{
	const AspectList &List( int nAspect )
	{
		const AspectList *p = ListFor( nAspect );
		REQUIRE( p != nullptr );
		return *p;
	}

	// The 1-based option value's actual size, for readable assertions.
	SizePreset At( const AspectList &list, int nValue )
	{
		REQUIRE( nValue >= 1 );
		REQUIRE( (size_t)nValue <= list.nSizes );
		return list.pSizes[ nValue - 1 ];
	}

	struct TempConfigHome
	{
		std::filesystem::path dir;

		TempConfigHome()
		{
			std::string sTemplate = ( std::filesystem::temp_directory_path() /
				"gamescope-ritz-res-XXXXXX" ).string();
			char *pszResult = mkdtemp( sTemplate.data() );
			REQUIRE( pszResult != nullptr );
			dir = pszResult;
			setenv( "XDG_CONFIG_HOME", dir.c_str(), 1 );
			ResetSessionRoutingForTests();
		}

		~TempConfigHome()
		{
			FlushPendingWrites();
			std::error_code ec;
			std::filesystem::remove_all( dir, ec );
			unsetenv( "XDG_CONFIG_HOME" );
			ResetSessionRoutingForTests();
		}
	};
}

TEST_CASE( "every shape's list is described by its own ratio", "[resolution]" )
{
	for ( const AspectList &list : kAspectLists )
	{
		REQUIRE( list.nSizes > 0 );
		for ( size_t i = 0; i < list.nSizes; i++ )
		{
			const SizePreset &s = list.pSizes[i];
			// Every entry of a list must classify as that list's own shape,
			// or the reflection rule would show a size under a shape it was
			// not picked from.
			REQUIRE( NearestAspect( s.nWidth, s.nHeight ) == list.nAspect );
			REQUIRE( MatchSizePreset( list.pSizes, list.nSizes, s.nWidth, s.nHeight ) == (int)i + 1 );
		}
	}
}

TEST_CASE( "MatchSizePreset finds only exact sizes", "[resolution]" )
{
	const AspectList &l = List( kAspect16x9 );
	REQUIRE( MatchSizePreset( l.pSizes, l.nSizes, 1920, 1080 ) == 3 );
	REQUIRE( MatchSizePreset( l.pSizes, l.nSizes, 1919, 1080 ) == -1 );
	REQUIRE( MatchSizePreset( l.pSizes, l.nSizes, 1440, 1080 ) == -1 );   // 4:3, not this list
}

TEST_CASE( "NearestAspect classifies within tolerance and refuses outside it", "[resolution]" )
{
	REQUIRE( NearestAspect( 1920, 1080 ) == kAspect16x9 );
	REQUIRE( NearestAspect( 1440, 1080 ) == kAspect4x3 );
	REQUIRE( NearestAspect( 1680, 1050 ) == kAspect16x10 );
	// True 21:9 (2.333) against the list's nominal 64:27 (2.370) -- inside
	// the 3 % tolerance, which is why that tolerance exists.
	REQUIRE( NearestAspect( 2520, 1080 ) == kAspect21x9 );
	// A square and a 32:9 superwide belong to no shape.
	REQUIRE( NearestAspect( 1000, 1000 ) == -1 );
	REQUIRE( NearestAspect( 5120, 1440 ) == -1 );
	REQUIRE( NearestAspect( 0, 1080 ) == -1 );
}

TEST_CASE( "a shape switch lands on the closest size BY HEIGHT", "[resolution]" )
{
	// The user's own rule (2026-09-06 item 8): "changing the aspect ratio
	// automatically picks the closest resolution, measured by height".
	// 1080-tall in every shape:
	REQUIRE( At( List( kAspect4x3 ),   ClosestByHeight( List( kAspect4x3 ),   1080 ) )
		.nHeight == 1080 );
	REQUIRE( At( List( kAspect4x3 ),   ClosestByHeight( List( kAspect4x3 ),   1080 ) )
		.nWidth == 1440 );
	REQUIRE( At( List( kAspect21x9 ),  ClosestByHeight( List( kAspect21x9 ),  1080 ) )
		.nWidth == 2560 );
	// 16:10 has no 1080 entry; 1050 is 30 away and 1200 is 120 away.
	REQUIRE( At( List( kAspect16x10 ), ClosestByHeight( List( kAspect16x10 ), 1080 ) )
		.nHeight == 1050 );
	// From a 4K-tall mode, and from something far below every entry.
	REQUIRE( At( List( kAspect16x9 ),  ClosestByHeight( List( kAspect16x9 ),  2160 ) )
		.nHeight == 2160 );
	REQUIRE( At( List( kAspect16x9 ),  ClosestByHeight( List( kAspect16x9 ),  200 ) )
		.nHeight == 720 );
	// The pick is always a real entry, never Custom, for every shape and a
	// wide sweep of heights.
	for ( const AspectList &list : kAspectLists )
		for ( int nHeight = 240; nHeight <= 4320; nHeight += 37 )
			REQUIRE( ClosestByHeight( list, nHeight ) != kSizeCustom );
}

TEST_CASE( "a height tie goes to the wider size", "[resolution]" )
{
	// No shipped list has two entries of the same height, so the tie rule is
	// pinned on a table that does. Larger width wins whichever order the
	// entries are in, so the answer cannot come from table order by accident.
	const SizePreset kNarrowFirst[] = { { 1440, 1080 }, { 1920, 1080 } };
	const SizePreset kWideFirst[]   = { { 1920, 1080 }, { 1440, 1080 } };
	REQUIRE( ClosestByHeight( kNarrowFirst, std::size( kNarrowFirst ), 1080 ) == 2 );
	REQUIRE( ClosestByHeight( kWideFirst,   std::size( kWideFirst ),   1080 ) == 1 );
	// An equidistant pair on opposite sides is a tie too: 1000 is 80 from
	// both 920 and 1080, and the wider of the two wins.
	const SizePreset kEitherSide[] = { { 1226, 920 }, { 1920, 1080 } };
	REQUIRE( ClosestByHeight( kEitherSide, std::size( kEitherSide ), 1000 ) == 2 );
	// An empty table has nothing to pick.
	REQUIRE( ClosestByHeight( nullptr, 0, 1080 ) == kSizeCustom );
}

TEST_CASE( "a config written by the four-row UI still loads", "[resolution]" )
{
	// The removed rows (display.resolution.preset_16_9 / _4_3 / _16_10 /
	// _21_9) never declared a config key -- only nested_width/height/
	// refresh_hz were ever persisted -- so "migration" is that the stored
	// mode survives and any stray key is ignored rather than fatal.
	TempConfigHome home;

	const std::string sPath = ProfilePath( "Old" );
	std::filesystem::create_directories( std::filesystem::path( sPath ).parent_path() );
	{
		std::ofstream f( sPath );
		f << R"({
			"name": "Old",
			"gamescope": {
				"nested_width": 1440,
				"nested_height": 1080,
				"nested_refresh_hz": 144,
				"preset_16_9": 3,
				"preset_4_3": 3,
				"preset_16_10": 0,
				"preset_21_9": 0,
				"output_size": 2,
				"output_size_width": 2560,
				"output_size_height": 1440
			}
		})";
	}

	auto loaded = LoadProfile( "Old" );
	REQUIRE( loaded.has_value() );
	REQUIRE( loaded->gamescope.nested_width == 1440 );
	REQUIRE( loaded->gamescope.nested_height == 1080 );
	REQUIRE( loaded->gamescope.nested_refresh_hz == 144 );

	// And that stored mode still classifies as the shape and entry the one
	// row now shows: 4:3, "1440 x 1080".
	REQUIRE( NearestAspect( loaded->gamescope.nested_width, loaded->gamescope.nested_height ) == kAspect4x3 );
	REQUIRE( MatchSizePreset( List( kAspect4x3 ).pSizes, List( kAspect4x3 ).nSizes,
		loaded->gamescope.nested_width, loaded->gamescope.nested_height ) == 3 );
}

TEST_CASE( "ClassifyAspect matches an exact preset to its shape", "[resolution]" )
{
	// Every real entry on every list classifies as that list's own shape --
	// same guarantee as MatchSizePreset()/NearestAspect() above, through the
	// function PanelDisplay.cpp's CurrentAspect() actually calls now.
	REQUIRE( ClassifyAspect( 1920, 1080, 2560, 1440 ) == kAspect16x9 );
	REQUIRE( ClassifyAspect( 1440, 1080, 2560, 1440 ) == kAspect4x3 );
	REQUIRE( ClassifyAspect( 1680, 1050, 2560, 1440 ) == kAspect16x10 );
	REQUIRE( ClassifyAspect( 2560, 1080, 2560, 1440 ) == kAspect21x9 );
}

TEST_CASE( "ClassifyAspect reports Native only for the output's own size", "[resolution]" )
{
	// 1366x768 is a real laptop panel size that matches no shape's list, so
	// list membership can't be the reason this comes back Native.
	REQUIRE( ClassifyAspect( 1366, 768, 1366, 768 ) == kAspectNative );
	// A size equal to the output that ALSO happens to be an exact list
	// entry is still read as that shape first -- an explicit pick from the
	// list is what "trusts" a size as a shape, and this is the no-pick
	// fallback, so list membership wins over the coincidence.
	REQUIRE( ClassifyAspect( 1920, 1080, 1920, 1080 ) == kAspect16x9 );
}

TEST_CASE( "ClassifyAspect flips an unmatched size to Custom, never to a nearby shape",
           "[resolution]" )
{
	// requests-2026-09-07 item 3, the user: "Setting Aspect ratio to custom
	// should allow the editing of the custom resolution ... it shouldnt be
	// visible to the user" [as a "Custom" entry in the Resolution list]. A
	// size that is off by one pixel from a real entry -- or merely close to
	// a shape's nominal ratio the way true 21:9 (2.333) sits close to the
	// list's nominal 64:27 (2.370), which NearestAspect()'s 3% tolerance
	// used to forgive -- no longer has a "shape + Custom" state to land in,
	// because the Resolution dropdown has no Custom entry left to represent
	// it with. It must be Custom, not the nearby shape.
	REQUIRE( ClassifyAspect( 1919, 1080, 2560, 1440 ) == kAspectCustom );
	REQUIRE( ClassifyAspect( 1600, 1200, 2560, 1440 ) == kAspectCustom );   // 4:3 ratio, not a listed 4:3 size
	REQUIRE( ClassifyAspect( 2520, 1080, 2560, 1440 ) == kAspectCustom );   // true 21:9, within NearestAspect's old tolerance
	REQUIRE( ClassifyAspect( 1000, 1000, 2560, 1440 ) == kAspectCustom );   // square, no shape at all
	// A launch-time -w 1600 -h 1200 with no matching output size either.
	REQUIRE( ClassifyAspect( 1600, 1200, 1920, 1080 ) == kAspectCustom );
}

TEST_CASE( "FormatLiveLine reads \"nested W x H@Hz . output W x H@Hz\"", "[resolution]" )
{
	// requests-2026-09-07 item 4, the user: 'Use the terminology "nested" and
	// "output". Make the line like this: "nested <nested_res>@<nested_refresh>
	// . output <output_res>@<output_refresh>"' (middle dot in the real
	// string, spelled out here to keep this file plain ASCII).
	REQUIRE( FormatLiveLine( 1920, 1080, 144, 2560, 1440, 60 )
		== "nested 1920x1080@144 \xC2\xB7 output 2560x1440@60" );
	// Following the host: nested refresh is computed as the effective (host)
	// rate by the caller before this function ever sees it, so the two
	// numbers legitimately match -- this function just prints what it's
	// given.
	REQUIRE( FormatLiveLine( 1280, 720, 60, 1280, 720, 60 )
		== "nested 1280x720@60 \xC2\xB7 output 1280x720@60" );
}
