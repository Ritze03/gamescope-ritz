#pragma once

// FrameGenFormat.h -- the pure half of "which frame-gen-ritz Format does this
// layer 0 get": (the game texture's VkFormat, is it YCbCr, its
// GamescopeAppTextureColorspace) -> (library Format, Transfer, white level,
// the format of the host's private ring / outputs), or the reason there is none.
//
// Header-only and free of rendervulkan.hpp so tests/test_framegen_format.cpp
// can pin the table without a GPU (superdoc/features/frame-generation.md,
// "HDR and 10-bit games").
//
// WHY the colourspace matters at all: the ring and outputs carry the game's
// own bits unchanged (cs_fg_copy.comp is an exact copy), and the substituted
// layer 0 keeps the game's colourspace tag, so the compositor decodes the
// generated frame exactly as it would the real one. The only thing the tag
// decides HERE is how the library MEASURES (RgbaF16: encoded values or linear
// light) -- the warp and blend always run in the picture's own space.

#include <cstdint>
#include <vulkan/vulkan.h>

#include "../../subprojects/FrameGen/gpu/framegen.h"

#include "FrameGenHost.h"
#include "gamescope_shared.h"

namespace fghost
{
	// What the host can create on this device, besides the 8-bit default.
	// Decided once at first use from the device (FrameGenHost.cpp): the
	// shaderStorageImageExtendedFormats feature (rgb10_a2 stores) and the
	// format features of the ring formats.
	struct FormatCaps
	{
		bool bRgb10 = false;   // A2B10G10R10 ring + library variant usable
		bool bF16 = false;     // R16G16B16A16_SFLOAT ring + library variant usable
	};

	// What kind of frame the host's ring holds; one library Interpolator per plan.
	struct FormatPlan
	{
		framegen::Format eFormat = framegen::Format::Rgba8;
		framegen::Transfer eTransfer = framegen::Transfer::Encoded;   // RgbaF16 only
		float flLinearWhiteNits = 80.0f;                              // RgbaF16 + Linear only
		VkFormat eRingVk = VK_FORMAT_B8G8R8A8_UNORM;                  // the ring/output format = InitInfo::viewFormat

		bool operator==( const FormatPlan &o ) const
		{
			return eFormat == o.eFormat && eTransfer == o.eTransfer && flLinearWhiteNits == o.flLinearWhiteNits;
		}
		bool operator!=( const FormatPlan &o ) const { return !( *this == o ); }
	};

	// A short tag for the Status line: "" for the plain 8-bit case.
	inline const char *PlanTag( const FormatPlan &p, GamescopeAppTextureColorspace eColorspace )
	{
		switch ( p.eFormat )
		{
			case framegen::Format::Rgb10:
				return ColorspaceIsHDR( eColorspace ) ? "HDR" : "10-bit";
			case framegen::Format::RgbaF16:
				return ColorspaceIsHDR( eColorspace ) ? "HDR" : "16-bit float";
			default:
				return "";
		}
	}

	// Why not / which. Ok -> *pPlan is filled.
	//
	//   B8G8R8A8 / R8G8B8A8 UNORM         any colourspace but PASSTHRU   Rgba8   (as before: the codes are the levels)
	//   A2B10G10R10 / A2R10G10B10 UNORM   any but PASSTHRU               Rgb10   (SDR-encoded or HDR10 PQ codes)
	//   R16G16B16A16_SFLOAT               SCRGB                          RgbaF16 + Linear, 1.0 = 80 nits
	//                                     anything else but PASSTHRU     RgbaF16 + Encoded
	//   YCbCr (NV12 ...)                  -                              YCbCr (not offered: see frame-generation.md)
	//   PASSTHRU                          -                              Hdr
	//   anything else (565, 16-bit UNORM) -                              Format
	//   10-bit / fp16 the device cannot do                              HdrFormat
	inline Unavailable ClassifyLayer( VkFormat eFormat, bool bYcbcr, GamescopeAppTextureColorspace eColorspace,
		const FormatCaps &caps, FormatPlan *pPlan )
	{
		if ( bYcbcr )
			return Unavailable::YCbCr;
		if ( eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU )
			return Unavailable::Hdr;

		FormatPlan plan;
		switch ( eFormat )
		{
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB:
			case VK_FORMAT_R8G8B8A8_SRGB:
				plan.eFormat = framegen::Format::Rgba8;
				plan.eRingVk = VK_FORMAT_B8G8R8A8_UNORM;
				break;

			case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
			case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
				if ( !caps.bRgb10 )
					return Unavailable::HdrFormat;
				plan.eFormat = framegen::Format::Rgb10;
				// Always A2B10G10R10, whatever the game's order: it is the one with
				// storage support everywhere (A2R10G10B10 storage is optional and
				// NVIDIA lacks it), and the copy samples the logical rgba anyway.
				plan.eRingVk = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
				break;

			case VK_FORMAT_R16G16B16A16_SFLOAT:
				if ( !caps.bF16 )
					return Unavailable::HdrFormat;
				plan.eFormat = framegen::Format::RgbaF16;
				plan.eRingVk = VK_FORMAT_R16G16B16A16_SFLOAT;
				if ( eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB )
				{
					plan.eTransfer = framegen::Transfer::Linear;
					plan.flLinearWhiteNits = 80.0f;   // scRGB: 1.0 = 80 nits (colorimetry.h c_scRGBLightScale)
				}
				break;

			default:
				return Unavailable::Format;
		}

		if ( pPlan )
			*pPlan = plan;
		return Unavailable::Ok;
	}
}
