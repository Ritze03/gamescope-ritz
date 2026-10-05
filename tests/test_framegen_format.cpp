// Unit tests for FrameGen/FrameGenFormat.h -- the pure (VkFormat, colourspace) ->
// (frame-gen-ritz Format, Transfer, ring format) table of HDR / 10-bit frame
// generation (superdoc/features/frame-generation.md, "HDR and 10-bit games").
// No GPU, no compositor.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>

#include "FrameGen/FrameGenFormat.h"

using namespace fghost;
using framegen::Format;
using framegen::Transfer;

namespace
{
	constexpr GamescopeAppTextureColorspace kSrgb = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
	constexpr GamescopeAppTextureColorspace kLinear = GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;
	constexpr GamescopeAppTextureColorspace kScrgb = GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB;
	constexpr GamescopeAppTextureColorspace kPq = GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ;
	constexpr GamescopeAppTextureColorspace kPass = GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU;
	const FormatCaps kAll{ true, true };
}

TEST_CASE( "framegen format: 8-bit RGB stays Rgba8 in every non-passthrough colourspace", "[framegen_format]" )
{
	for ( VkFormat f : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM } )
	{
		for ( auto cs : { kSrgb, kLinear, kScrgb, kPq } )
		{
			FormatPlan p;
			REQUIRE( ClassifyLayer( f, false, cs, FormatCaps{}, &p ) == Unavailable::Ok );
			CHECK( p.eFormat == Format::Rgba8 );
			CHECK( p.eRingVk == VK_FORMAT_B8G8R8A8_UNORM );
		}
	}
}

TEST_CASE( "framegen format: 10-bit RGB is Rgb10 with an A2B10G10R10 ring", "[framegen_format]" )
{
	for ( VkFormat f : { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32 } )
	{
		for ( auto cs : { kSrgb, kPq } )
		{
			FormatPlan p;
			REQUIRE( ClassifyLayer( f, false, cs, kAll, &p ) == Unavailable::Ok );
			CHECK( p.eFormat == Format::Rgb10 );
			CHECK( p.eRingVk == VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
			CHECK( std::string( PlanTag( p, cs ) ) == ( cs == kPq ? "HDR" : "10-bit" ) );
		}
		CHECK( ClassifyLayer( f, false, kPq, FormatCaps{ false, true }, nullptr ) == Unavailable::HdrFormat );
	}
}

TEST_CASE( "framegen format: fp16 scRGB is linear at 80 nits, other fp16 is encoded", "[framegen_format]" )
{
	FormatPlan p;
	REQUIRE( ClassifyLayer( VK_FORMAT_R16G16B16A16_SFLOAT, false, kScrgb, kAll, &p ) == Unavailable::Ok );
	CHECK( p.eFormat == Format::RgbaF16 );
	CHECK( p.eTransfer == Transfer::Linear );
	CHECK( p.flLinearWhiteNits == 80.0f );
	CHECK( p.eRingVk == VK_FORMAT_R16G16B16A16_SFLOAT );
	CHECK( std::string( PlanTag( p, kScrgb ) ) == "HDR" );

	for ( auto cs : { kSrgb, kLinear, kPq } )
	{
		FormatPlan q;
		REQUIRE( ClassifyLayer( VK_FORMAT_R16G16B16A16_SFLOAT, false, cs, kAll, &q ) == Unavailable::Ok );
		CHECK( q.eTransfer == Transfer::Encoded );
		CHECK( q != p );   // a transfer change restarts the interpolator
	}
	CHECK( ClassifyLayer( VK_FORMAT_R16G16B16A16_SFLOAT, false, kScrgb, FormatCaps{ true, false }, nullptr ) == Unavailable::HdrFormat );
}

TEST_CASE( "framegen format: refusals keep their specific reason", "[framegen_format]" )
{
	CHECK( ClassifyLayer( VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, true, kSrgb, kAll, nullptr ) == Unavailable::YCbCr );
	CHECK( ClassifyLayer( VK_FORMAT_B8G8R8A8_UNORM, false, kPass, kAll, nullptr ) == Unavailable::Hdr );
	CHECK( ClassifyLayer( VK_FORMAT_R5G6B5_UNORM_PACK16, false, kSrgb, kAll, nullptr ) == Unavailable::Format );
	CHECK( ClassifyLayer( VK_FORMAT_R16G16B16A16_UNORM, false, kSrgb, kAll, nullptr ) == Unavailable::Format );
}

TEST_CASE( "framegen format: an inert composite tracks each new real frame once, only where the library can take it", "[framegen_format]" )
{
	FormatPlan p8, p10;
	REQUIRE( ClassifyLayer( VK_FORMAT_B8G8R8A8_UNORM, false, kSrgb, kAll, &p8 ) == Unavailable::Ok );
	REQUIRE( ClassifyLayer( VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, kSrgb, kAll, &p10 ) == Unavailable::Ok );

	// LayerFitsHost: same plan, size and colourspace, classified Ok -- nothing else.
	CHECK( LayerFitsHost( Unavailable::Ok, p8, p8, 1920, 1080, 1920, 1080, true ) );
	CHECK_FALSE( LayerFitsHost( Unavailable::Format, p8, p8, 1920, 1080, 1920, 1080, true ) );   // a refused layer (D4)
	CHECK_FALSE( LayerFitsHost( Unavailable::Ok, p10, p8, 1920, 1080, 1920, 1080, true ) );      // the game changed format class
	CHECK_FALSE( LayerFitsHost( Unavailable::Ok, p8, p8, 1280, 720, 1920, 1080, true ) );        // a resolution change
	CHECK_FALSE( LayerFitsHost( Unavailable::Ok, p8, p8, 1920, 1080, 1920, 1080, false ) );      // SDR <-> HDR10 on the same surface

	// ShouldTrackUi: UI protection on, library live, layer fits, a frame it has not been handed.
	CHECK( ShouldTrackUi( true, true, true, 42, 41 ) );
	CHECK( ShouldTrackUi( true, true, true, 42, 0 ) );
	CHECK_FALSE( ShouldTrackUi( false, true, true, 42, 41 ) );    // UI protection off
	CHECK_FALSE( ShouldTrackUi( true, false, true, 42, 41 ) );    // no Interpolator: nothing to keep warm
	CHECK_FALSE( ShouldTrackUi( true, true, false, 42, 41 ) );    // unsupported layer
	CHECK_FALSE( ShouldTrackUi( true, true, true, 0, 41 ) );      // no frame
	// A repaint of the frame the library already has (cursor move, overlay), or one it
	// was just observed on the way into pass-through: never counted twice.
	CHECK_FALSE( ShouldTrackUi( true, true, true, 42, 42 ) );
}

// ---------------------------------------------------------------------------
//  UI protection (2026-10-05): the host's UiProt values ARE the library's, so
//  the host can cast between them, and the box-size range is the slider's.
// ---------------------------------------------------------------------------

TEST_CASE( "framegen ui: UiProt matches the library's UiProtection value for value", "[framegen_format]" )
{
	STATIC_REQUIRE( int( UiProt::Off ) == int( framegen::UiProtection::Off ) );
	STATIC_REQUIRE( int( UiProt::Crosshair ) == int( framegen::UiProtection::Crosshair ) );
	STATIC_REQUIRE( int( UiProt::WholeScreen ) == int( framegen::UiProtection::WholeScreen ) );
	STATIC_REQUIRE( int( UiProt::CrosshairV2 ) == int( framegen::UiProtection::CrosshairV2 ) );
	// Packed into two bits of the config word (FrameGenHost.cpp, bits 24-25).
	STATIC_REQUIRE( int( UiProt::CrosshairV2 ) <= 3 );
}

TEST_CASE( "framegen ui: the box size defaults and range", "[framegen_format]" )
{
	// The slider's 2.5% default and 0.5%..10% range, in tenths of a percent.
	CHECK( kUiBoxDefaultTenths == 25 );
	CHECK( kUiBoxMinTenths == 5 );
	CHECK( kUiBoxMaxTenths == 100 );
	// The library default must equal the host's: a host that set nothing would
	// otherwise protect a different box than the slider says.
	CHECK_THAT( framegen::Settings{}.uiBoxHeightFrac,
		Catch::Matchers::WithinAbs( double( kUiBoxDefaultTenths ) / 1000.0, 1e-6 ) );
}

TEST_CASE( "framegen ui: the box side the slider text and the library share", "[framegen_format]" )
{
	// The library's formula (Interpolator::boxSize, whose .cpp the unit tests do not
	// link): side = clamp(round(H * frac), 8, min(W, H)). 24 px at 960, 27 at 1080,
	// 36 at 1440, 54 at 2160 for the default.
	auto Side = []( uint32_t w, uint32_t h, int nTenths )
	{
		const int nSide = int( std::lround( double( h ) * double( nTenths ) / 1000.0 ) );
		return std::min( std::max( nSide, 8 ), int( std::min( w, h ) ) );
	};
	CHECK( Side( 1280, 960, kUiBoxDefaultTenths ) == 24 );
	CHECK( Side( 1920, 1080, kUiBoxDefaultTenths ) == 27 );
	CHECK( Side( 2560, 1440, kUiBoxDefaultTenths ) == 36 );
	CHECK( Side( 3840, 2160, kUiBoxDefaultTenths ) == 54 );
	CHECK( Side( 1920, 1080, kUiBoxMinTenths ) == 8 );      // 5.4 px, held at the library's 8 px floor
	CHECK( Side( 1920, 1080, kUiBoxMaxTenths ) == 108 );
}

TEST_CASE( "framegen: Pause at refresh rate is ignored in Target mode", "[framegen_format]" )
{
	CHECK( EffectiveCapAtRefresh( true, Mode::Fixed ) );
	CHECK_FALSE( EffectiveCapAtRefresh( false, Mode::Fixed ) );
	CHECK_FALSE( EffectiveCapAtRefresh( true, Mode::Target ) );
	CHECK_FALSE( EffectiveCapAtRefresh( false, Mode::Target ) );
}
