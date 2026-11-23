// Frame generation's Debug options, the pure parts (superdoc/features/frame-generation-debug.md): the DebugConfig word, the
// "hold the generated frame" decision of "Only generated frames", and the UI-box word with Whole screen V2's bit.
// Header-only and free of GPU code so tests/test_framegen_debug.cpp can run it.
//
// DebugConfig word (g_uDebug, its own atomic: the packed Config word is full):
//   bit 0      onlyGenerated
//   bit 1      stats
//   bits 2-5   view (reserved: 0 = off; the later library debug views, unused for now)
//   bits 6-31  unused, written as 0 and ignored on unpack
// UI-box word (g_uUiBox): bits 0-7 the crosshair box in tenths of a percent, bit 8 Whole screen V2 (the box then is the whole
// frame, whatever the tenths say; the tenths stay stored so the slider keeps its value).
#pragma once

#include <algorithm>
#include <cstdint>

#include "FrameGenHost.h"

namespace fghost
{
	// Longest a generated frame stays on screen in place of a real one. At fixed 2x the hold lasts one tick; with a Target
	// above what the game delivers, several. A bound so a stalled generator can never freeze the picture, and short enough
	// (100 ms = a 10 fps game's frame) that when the pacer starts passing real frames through (a scene cut) the held image
	// is not shown for long; the pacer's own `generating` flag covers the rest (the host passes it as bGenerating).
	constexpr uint64_t kHoldMaxNs = 100ull * 1000ull * 1000ull;

	// "Only generated frames": on a REAL tick (the request shows a real frame), show the last generated image again instead of
	// the real one. Never while nothing is generated (bGenerating false: pass-through, inert, failed synth), when no valid
	// output exists, or when the last generated image is older than kHoldMaxNs.
	inline bool ShouldHoldGenerated( bool bOn, bool bRealTick, bool bGenerating, bool bHaveSlot, uint64_t ulAgeNs )
	{
		return bOn && bRealTick && bGenerating && bHaveSlot && ulAgeNs <= kHoldMaxNs;
	}

	// A request shows the real frame when it has no pair (prevId 0) or sits at t >= 1 (Motion blur's real frame carries a prevId).
	inline bool IsRealTick( uint64_t ulPrevId, float flT )
	{
		return ulPrevId == 0 || flT >= 1.0f;
	}

	inline uint32_t PackDebug( const DebugConfig &d )
	{
		return ( d.onlyGenerated ? 1u : 0u ) | ( d.stats ? 2u : 0u ) | ( ( uint32_t( std::max( d.view, 0 ) ) & 0xFu ) << 2 );
	}

	inline DebugConfig UnpackDebug( uint32_t u )
	{
		DebugConfig d;
		d.onlyGenerated = ( u & 1u ) != 0;
		d.stats = ( u & 2u ) != 0;
		d.view = int( ( u >> 2 ) & 0xFu );
		return d;
	}

	constexpr uint32_t kUiWholeFrameBit = 1u << 8;
	// Whole screen V2: the library clamps an explicit box side to the frame, so this is the whole picture, non-square.
	constexpr uint32_t kWholeFrameBoxPx = 16384;

	inline uint32_t PackUiBox( int nTenths, bool bWholeFrame )
	{
		return uint32_t( std::min( std::max( nTenths, kUiBoxMinTenths ), kUiBoxMaxTenths ) ) | ( bWholeFrame ? kUiWholeFrameBit : 0u );
	}

	inline void UnpackUiBox( uint32_t u, int *pnTenths, bool *pbWholeFrame )
	{
		*pnTenths = std::min( std::max( int( u & 0xFFu ), kUiBoxMinTenths ), kUiBoxMaxTenths );
		*pbWholeFrame = ( u & kUiWholeFrameBit ) != 0;
	}

	// The one name of the UI-protection mode (stats overlay, capture header, logs): Whole screen V2 is Crosshair V2 plus the
	// whole-frame flag and must not read as plain "Crosshair V2".
	inline const char *UiModeName( UiProt eUi, bool bWholeFrame )
	{
		switch ( eUi )
		{
			case UiProt::Off:         return "Off";
			case UiProt::Crosshair:   return "Crosshair";
			case UiProt::WholeScreen: return "Whole screen";
			case UiProt::CrosshairV2: return bWholeFrame ? "Whole screen V2" : "Crosshair V2";
		}
		return "";
	}

	// The library's box settings for a mode: the fraction of the height, plus explicit pixel sides (0 = take it from the fraction).
	// Whole screen V2 = Crosshair V2 with the explicit box at the library's maximum.
	inline void UiBoxSettings( bool bCrosshairV2, bool bWholeFrame, int nTenths, float *pflFrac, uint32_t *puBoxW, uint32_t *puBoxH )
	{
		*pflFrac = float( nTenths ) / 1000.0f;
		const bool bWhole = bCrosshairV2 && bWholeFrame;
		*puBoxW = *puBoxH = bWhole ? kWholeFrameBoxPx : 0u;
	}
}
