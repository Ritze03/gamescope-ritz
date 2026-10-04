// Unit tests for FrameGen/FrameGenFormat.h -- the pure (VkFormat, colourspace) ->
// (frame-gen-ritz Format, Transfer, ring format) table of HDR / 10-bit frame
// generation (superdoc/features/frame-generation.md, "HDR and 10-bit games").
// No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>
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
