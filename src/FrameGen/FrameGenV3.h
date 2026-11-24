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
//   bits 52-55  v3Blend        0 off (V3's own rule), 1..2 = V3 Hybrid variants (HybridVariants.h; only
//                              1..kHybridBlendMax are written, today 1..2; four bits, the range is small today)
//   bits 56-63  unused, written as 0 and ignored on unpack
// The whole word is the applied key's comparison in EnsureReady(), so a change to either switch is noticed.
// `Why a sixth word:` the five existing words (config, UI box, Learned, reach, blur) and the 64-bit applied key in
// EnsureReady() have no free bit left, and four values at twelve bits need 48.
// `Why hundredths:` 0.35 = 35 / 100.0 round-trips exactly through float (the defaults must change nothing) and the
// overlay steps by 0.05. A field out of range falls back to the default on unpack (cannot happen from PackV3).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "FrameGenHost.h"
#include "HybridVariants.h"   // the variant table (v3Blend 1 = Fluidity, 2 = Consistency)
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

	// nBlendMax: the highest v3Blend that is written (anything else packs as 0); a parameter so tests can cover other bounds.
	inline uint64_t PackV3( const Config &c, int nBlendMax = kHybridBlendMax )
	{
		using namespace v3detail;
		return Hundredths( c.v3Smear, kV3SmearMin, kV3SmearMax, kV3SmearDefault )
			| ( Hundredths( c.v3OcclSoft, kV3OcclSoftMin, kV3OcclSoftMax, kV3OcclSoftDefault ) << 12 )
			| ( Hundredths( c.v3Unsure, kV3UnsureMin, kV3UnsureMax, kV3UnsureDefault ) << 24 )
			| ( Hundredths( c.v3SceneCut, kV3SceneCutMin, kV3SceneCutMax, kV3SceneCutDefault ) << 36 )
			| ( uint64_t( c.v3ColourWindow ) << 48 )
			| ( uint64_t( c.v3DetailSplit ) << 49 )
			| ( uint64_t( c.v3Guard >= 1 && c.v3Guard <= 2 ? c.v3Guard : 0 ) << 50 )
			| ( uint64_t( c.v3Blend >= 1 && c.v3Blend <= nBlendMax ? c.v3Blend : 0 ) << 52 );
	}

	inline void UnpackV3( uint64_t u, Config *c, int nBlendMax = kHybridBlendMax )
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
		{
			const uint64_t uBlend = ( u >> 52 ) & 15u;
			c->v3Blend = uBlend <= uint64_t( nBlendMax ) ? int( uBlend ) : 0;
		}
	}

	// V3 Hybrid (2026-11-23): the config's motion_version string plus the variant key become the host's Motion and
	// blend. "v3_hybrid" is Motion::V3 with a blend of 1..kHybridBlendMax (the stored variant strings of HybridVariants.h; anything else is Fluidity), so every
	// Motion::V3 check applies to it unchanged. Every other version has blend 0, whatever the variant says; an unknown
	// motion string is V1 (the schema has already filtered it). Pure, so tests/test_framegen_v3.cpp pins the mapping.
	struct MotionChoice
	{
		Motion motion = Motion::V1;
		int blend = 0;
	};

	inline MotionChoice MotionFromKeys( std::string_view svMotion, std::string_view svVariant )
	{
		if ( svMotion == "v2" )
			return { Motion::V2, 0 };
		if ( svMotion == "learned" )
			return { Motion::Learned, 0 };
		if ( svMotion == "v3" )
			return { Motion::V3, 0 };
		if ( svMotion == "v3_hybrid" )
			return { Motion::V3, std::max( 1, HybridBlendFromKey( svVariant ) ) };
		return { Motion::V1, 0 };
	}

	// "" for blend 0 (plain V3, nothing to say), else the variant label from HybridVariants.h, "Fluidity" / "Consistency" (logs and the capture header's `blend=`).
	// BlendLabel() adds the honest state: "Fluidity" while the learned blend runs, "Fluidity(off)" when the variant is selected but
	// the library runs V3's rule (a format without a blend build, the detail split, a V3 fallback).
	inline const char *BlendText( int nBlend )
	{
		return HybridLabel( nBlend );
	}

	inline std::string BlendLabel( int nBlend, bool bActive )
	{
		const std::string s = BlendText( nBlend );
		return s.empty() || bActive ? s : s + "(off)";
	}
}
