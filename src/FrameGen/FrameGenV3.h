// Motion version 3's four runtime settings (Config::v3Smear / v3OcclSoft / v3Unsure / v3SceneCut) packed into ONE
// 64-bit word, so the render thread reads a consistent set (FrameGenHost.cpp's g_ulV3). Header-only and pure so
// tests/test_framegen_v3.cpp can round-trip it without the host's GPU code.
//
// Layout (the four values in hundredths, twelve bits each; the two switches one bit each; the guard two bits):
//   bits  0-11  v3Smear        0..100   (0..1)
//   bits 12-23  v3OcclSoft     25..400  (0.25..4)
//   bits 24-35  v3Unsure       25..400  (0.25..4)
//   bits 36-47  v3SceneCut     0..400   (0..4, 0 = never a scene cut)
//   bit  48     v3ColourWindow (motion search weighted by colour similarity)
//   bit  49     v3DetailSplit  (fine detail taken from one frame)
//   bits 50-51  v3Guard        0 Off, 1 Guard, 2 V2 motion (3 unpacks as 0)
//   bits 52-63  unused, written as 0 and ignored on unpack
// The whole word is the applied key's comparison in EnsureReady(), so a change to either switch is noticed.
// `Why a sixth word:` the five existing words (config, UI box, Learned, reach, blur) and the 64-bit applied key in
// EnsureReady() have no free bit left, and four values at twelve bits need 48.
// `Why hundredths:` 0.35 = 35 / 100.0 round-trips exactly through float (the defaults must change nothing) and the
// overlay steps by 0.05. A field out of range falls back to the default on unpack (cannot happen from PackV3).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "FrameGenHost.h"
#include "FrameGenReach.h"   // ReachIsFinite(): the bit-pattern finite test (the project builds with -ffast-math)

namespace fghost
{
	namespace v3detail
	{
		inline uint64_t Hundredths( float v, float flMin, float flMax, float flDefault )
		{
			const float f = ReachIsFinite( v ) ? std::clamp( v, flMin, flMax ) : flDefault;
			return uint64_t( std::llround( double( f ) * 100.0 ) );
		}

		inline float FromHundredths( uint64_t u, uint64_t uMin, uint64_t uMax, float flDefault )
		{
			return u >= uMin && u <= uMax ? float( double( u ) / 100.0 ) : flDefault;
		}
	}

	inline uint64_t PackV3( const Config &c )
	{
		using namespace v3detail;
		return Hundredths( c.v3Smear, kV3SmearMin, kV3SmearMax, kV3SmearDefault )
			| ( Hundredths( c.v3OcclSoft, kV3OcclSoftMin, kV3OcclSoftMax, kV3OcclSoftDefault ) << 12 )
			| ( Hundredths( c.v3Unsure, kV3UnsureMin, kV3UnsureMax, kV3UnsureDefault ) << 24 )
			| ( Hundredths( c.v3SceneCut, kV3SceneCutMin, kV3SceneCutMax, kV3SceneCutDefault ) << 36 )
			| ( uint64_t( c.v3ColourWindow ) << 48 )
			| ( uint64_t( c.v3DetailSplit ) << 49 )
			| ( uint64_t( c.v3Guard >= 1 && c.v3Guard <= 2 ? c.v3Guard : 0 ) << 50 );
	}

	inline void UnpackV3( uint64_t u, Config *c )
	{
		using namespace v3detail;
		c->v3Smear = FromHundredths( u & 0xFFFu, 0, 100, kV3SmearDefault );
		c->v3OcclSoft = FromHundredths( ( u >> 12 ) & 0xFFFu, 25, 400, kV3OcclSoftDefault );
		c->v3Unsure = FromHundredths( ( u >> 24 ) & 0xFFFu, 25, 400, kV3UnsureDefault );
		c->v3SceneCut = FromHundredths( ( u >> 36 ) & 0xFFFu, 0, 400, kV3SceneCutDefault );
		c->v3ColourWindow = ( ( u >> 48 ) & 1u ) != 0;
		c->v3DetailSplit = ( ( u >> 49 ) & 1u ) != 0;
		{
			const uint64_t uGuard = ( u >> 50 ) & 3u;
			c->v3Guard = uGuard <= 2 ? int( uGuard ) : 0;
		}
	}
}
