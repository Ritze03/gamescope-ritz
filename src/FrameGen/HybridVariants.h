// V3 Hybrid's variant list as ONE table (pure, no includes of the host, so Config and the overlay panel can use it).
// The library's Settings::v3Blend = 1 + 3 * set + net (net 0 / 1 / 2 = A / B / C); 0 = no learned blend. Each set has a
// label prefix and a stored-string prefix, so the labels are A, B, C, A-A, .. and the stored strings a, b, c, a_a, ..
// kHybridSetsActive is how many sets the overlay row offers, the config accepts and the host passes on; the table
// itself holds every set the host knows names for. kHybridSetsActive must equal min( framegen::kV3BlendMax / 3,
// kHybridSetsTotal ): FrameGenHost.cpp static_asserts it (this header cannot include the library's, which pulls in
// Vulkan), so a pin with more sets fails the build there and raising kHybridSetsActive is the only code edit it needs. See superdoc/features/frame-generation-v3-hybrid.md.
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace fghost
{
	constexpr int kHybridSetsTotal  = 5;   // sets with a name in kHybridSets
	constexpr int kHybridSetsActive = 2;   // sets in use today (A..C, A-A..A-C); the one line to change, checked against the library
	constexpr int kHybridNets       = 3;   // A / B / C per set

	struct HybridSet
	{
		const char *pszLabel;   // label prefix ("" / "A-" / "AR-" ..)
		const char *pszKey;     // stored-string prefix ("" / "a_" / "ar_" ..)
	};
	constexpr HybridSet kHybridSets[ kHybridSetsTotal ] = {
		{ "", "" }, { "A-", "a_" }, { "AR-", "ar_" }, { "AR2-", "ar2_" }, { "AR3-", "ar3_" } };

	constexpr int kHybridBlendCount = kHybridSetsTotal * kHybridNets;   // 15, the highest value the table can name
	constexpr int HybridBlendMax( int nSets ) { return nSets * kHybridNets; }
	constexpr int kHybridBlendMax = HybridBlendMax( kHybridSetsActive );   // highest v3Blend that may reach the library
	constexpr int HybridBlend( int nSet, int nNet ) { return 1 + kHybridNets * nSet + nNet; }

	namespace hybriddetail
	{
		// Index 0 is "" (no blend); 1..15 are the names, built from the label (bKey false) or the key prefix.
		inline std::array<std::string, 1 + kHybridBlendCount> Names( bool bKey )
		{
			std::array<std::string, 1 + kHybridBlendCount> a;
			for ( int s = 0; s < kHybridSetsTotal; s++ )
				for ( int n = 0; n < kHybridNets; n++ )
					a[ HybridBlend( s, n ) ] = std::string( bKey ? kHybridSets[ s ].pszKey : kHybridSets[ s ].pszLabel )
						+ char( ( bKey ? 'a' : 'A' ) + n );
			return a;
		}
	}

	// Label / stored string of a blend value 1..15; "" outside that range (also for 0).
	inline const std::string &HybridLabel( int nBlend )
	{
		static const auto kAll = hybriddetail::Names( false );
		return kAll[ nBlend >= 1 && nBlend <= kHybridBlendCount ? nBlend : 0 ];
	}

	inline const std::string &HybridKey( int nBlend )
	{
		static const auto kAll = hybriddetail::Names( true );
		return kAll[ nBlend >= 1 && nBlend <= kHybridBlendCount ? nBlend : 0 ];
	}

	// Blend value of a stored string, 0 when it is unknown or names a set beyond nSets.
	inline int HybridBlendFromKey( std::string_view svKey, int nSets = kHybridSetsActive )
	{
		for ( int b = 1; b <= HybridBlendMax( nSets ) && b <= kHybridBlendCount; b++ )
			if ( svKey == HybridKey( b ) )
				return b;
		return 0;
	}
}
