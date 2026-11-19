// The motion search reach (Config::searchRadius / searchRadiusTop / pyramidMinSide / searchPenalty) packed
// into ONE 32-bit word, so the render thread reads a consistent set (FrameGenHost.cpp's g_uReach). Header-only
// and pure so tests/test_framegen_reach.cpp can round-trip it without the host's GPU code.
//
// Layout: bits 0-3 search radius, 4-7 radius at the coarsest level (1..12 each), 8-16 pyramidMinSide (16..256:
// 256 needs nine bits), 17-31 search penalty in hundredths (0..25500, fifteen bits): exactly 32 bits.
// `Why a fifth word:` the four existing words (config, UI box, Learned, blur) and the 64-bit applied key in
// EnsureReady() have no free bit left.
// `Why hundredths:` 0.5 = 50 / 100.0 round-trips exactly (the default must change nothing) and the overlay steps
// by 0.05. A field out of range falls back to the default on unpack (cannot happen from PackReach).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "FrameGenHost.h"

namespace fghost
{
	inline uint32_t PackReach( const Config &c )
	{
		const uint32_t uR = uint32_t( std::clamp( c.searchRadius, kSearchRadiusMin, kSearchRadiusMax ) );
		const uint32_t uRt = uint32_t( std::clamp( c.searchRadiusTop, kSearchRadiusMin, kSearchRadiusMax ) );
		const uint32_t uPyr = uint32_t( std::clamp( c.pyramidMinSide, kPyramidMinSideMin, kPyramidMinSideMax ) );
		const float flPen = std::isfinite( c.searchPenalty )
			? std::clamp( c.searchPenalty, 0.0f, kSearchPenaltyMax ) : kSearchPenaltyDefault;
		const uint32_t uPen = uint32_t( std::lround( double( flPen ) * 100.0 ) );
		return uR | ( uRt << 4 ) | ( uPyr << 8 ) | ( uPen << 17 );
	}

	inline void UnpackReach( uint32_t u, Config *c )
	{
		const int nR = int( u & 0xFu ), nRt = int( ( u >> 4 ) & 0xFu ), nPyr = int( ( u >> 8 ) & 0x1FFu );
		const uint32_t uPen = ( u >> 17 ) & 0x7FFFu;
		c->searchRadius = nR >= kSearchRadiusMin && nR <= kSearchRadiusMax ? nR : kSearchRadiusDefault;
		c->searchRadiusTop = nRt >= kSearchRadiusMin && nRt <= kSearchRadiusMax ? nRt : kSearchRadiusTopDefault;
		c->pyramidMinSide = nPyr >= kPyramidMinSideMin && nPyr <= kPyramidMinSideMax ? nPyr : kPyramidMinSideDefault;
		c->searchPenalty = uPen <= 25500u ? float( double( uPen ) / 100.0 ) : kSearchPenaltyDefault;
	}
}
