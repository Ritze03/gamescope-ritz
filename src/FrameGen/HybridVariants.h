// V3 Hybrid's variant list: a plain table of the library's learned blends (Settings::v3Blend; 0 = no learned blend, the
// colour rule). Pure, no includes of the host or the library's header (it pulls in Vulkan), so Config, the overlay panel
// and the host all use it; FrameGenHost.cpp static_asserts the values against framegen::kV3BlendFluidity,
// kV3BlendConsistency and kV3BlendMax. See superdoc/features/frame-generation-v3-hybrid.md.
#pragma once

#include <string>
#include <string_view>

namespace fghost
{
	struct HybridVariant
	{
		const char *pszLabel;   // shown in the menu, the log, the status text and the capture header
		const char *pszKey;     // stored in framegen.v3_hybrid_variant
		int         nBlend;     // the library's v3Blend
	};
	constexpr HybridVariant kHybridVariants[] = {
		{ "Fluidity",    "fluidity",    1 },
		{ "Consistency", "consistency", 2 } };
	constexpr int kHybridBlendMax = int( sizeof( kHybridVariants ) / sizeof( kHybridVariants[ 0 ] ) );   // highest v3Blend that may reach the library

	// Label / stored string of a blend value 1..kHybridBlendMax; "" outside that range (also for 0).
	inline const char *HybridLabel( int nBlend )
	{
		return nBlend >= 1 && nBlend <= kHybridBlendMax ? kHybridVariants[ nBlend - 1 ].pszLabel : "";
	}

	inline const char *HybridKey( int nBlend )
	{
		return nBlend >= 1 && nBlend <= kHybridBlendMax ? kHybridVariants[ nBlend - 1 ].pszKey : "";
	}

	// Blend value of a stored string, 0 when it is unknown.
	inline int HybridBlendFromKey( std::string_view svKey )
	{
		for ( const HybridVariant &v : kHybridVariants )
			if ( svKey == v.pszKey )
				return v.nBlend;
		return 0;
	}

	// Saved profiles of the fifteen-variant period (2026-11-23 .. 2026-11-24) hold a, b, c, a_a .. ar4_c: the two that were
	// kept map to their successors, every other string (old or unknown) is the default. Pure, so tests pin it.
	inline std::string HybridKeyFromStored( std::string_view svStored )
	{
		if ( svStored == "a_c" )
			return "fluidity";
		if ( svStored == "ar4_c" )
			return "consistency";
		return HybridBlendFromKey( svStored ) != 0 ? std::string( svStored ) : std::string( kHybridVariants[ 0 ].pszKey );
	}
}
