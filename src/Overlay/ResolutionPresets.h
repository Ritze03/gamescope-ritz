#pragma once

// The Resolution area's pure half: the aspect shapes, their size lists, and
// the two classifications the UI makes from a live width/height. Split out of
// PanelDisplay.cpp so it can be unit-tested without ImGui, a backend or a
// compositor (tests/test_resolution.cpp) -- the same split
// Overlay/CrosshairMath.h already makes for the crosshair's geometry.
//
// See superdoc/features/resolution-and-refresh.md for the behaviour these
// numbers implement, including why picking a shape now APPLIES a size
// (requests-2026-09-06 item 8) and why there is one size row rather than one
// per shape (item 7).

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>

namespace gamescope::resolution
{
	struct SizePreset { int nWidth, nHeight; };

	enum Aspect : int
	{
		kAspectNative = 0,
		kAspect16x9   = 1,
		kAspect4x3    = 2,
		kAspect16x10  = 3,
		kAspect21x9   = 4,
		kAspectCustom = 5,
	};

	// The one size row's option values: 1..N index the shape's table,
	// kSizeCustom hands the size to the Width/Height steppers.
	static constexpr int kSizeCustom = 0;

	inline const SizePreset kSizes16x9[]  = { { 3840, 2160 }, { 2560, 1440 }, { 1920, 1080 }, { 1600, 900 }, { 1280, 720 } };
	inline const SizePreset kSizes4x3[]   = { { 2880, 2160 }, { 1920, 1440 }, { 1440, 1080 }, { 1280, 960 } };
	inline const SizePreset kSizes16x10[] = { { 3840, 2400 }, { 2560, 1600 }, { 1920, 1200 }, { 1680, 1050 }, { 1440, 900 }, { 1280, 800 } };
	inline const SizePreset kSizes21x9[]  = { { 5120, 2160 }, { 3440, 1440 }, { 2560, 1080 } };

	struct AspectList
	{
		int               nAspect;
		float             flRatio;      // nominal; 21:9 panels are really ~2.37, hence the tolerance below
		const SizePreset *pSizes;
		size_t            nSizes;
	};

	inline const AspectList kAspectLists[] = {
		{ kAspect16x9,  16.0f / 9.0f,  kSizes16x9,  std::size( kSizes16x9 )  },
		{ kAspect4x3,   4.0f / 3.0f,   kSizes4x3,   std::size( kSizes4x3 )   },
		{ kAspect16x10, 16.0f / 10.0f, kSizes16x10, std::size( kSizes16x10 ) },
		{ kAspect21x9,  64.0f / 27.0f, kSizes21x9,  std::size( kSizes21x9 )  },
	};

	// How far a live w/h may sit from a shape's nominal ratio and still be
	// called that shape. 3% covers true 21:9 (2.333) against the 2.37-2.39
	// the list's own sizes have, while 16:10 (1.6) and 16:9 (1.78) stay 11%
	// apart.
	inline constexpr float kAspectTolerance = 0.03f;

	inline const AspectList *ListFor( int nAspect )
	{
		for ( const AspectList &list : kAspectLists )
			if ( list.nAspect == nAspect )
				return &list;
		return nullptr;
	}

	// 1-based index of an exact match in a shape's table, or -1.
	inline int MatchSizePreset( const SizePreset *pPresets, size_t nPresets, int nWidth, int nHeight )
	{
		for ( size_t i = 0; i < nPresets; i++ )
			if ( pPresets[i].nWidth == nWidth && pPresets[i].nHeight == nHeight )
				return (int)i + 1;
		return -1;
	}

	// The shape whose nominal ratio the size sits within kAspectTolerance of,
	// or -1. Nearest wins if two ever qualified (none do at 3%).
	inline int NearestAspect( int nWidth, int nHeight )
	{
		if ( nWidth <= 0 || nHeight <= 0 )
			return -1;
		const float flRatio = (float)nWidth / (float)nHeight;
		int nBest = -1;
		float flBestErr = kAspectTolerance;
		for ( const AspectList &list : kAspectLists )
		{
			const float flErr = std::fabs( flRatio - list.flRatio ) / list.flRatio;
			if ( flErr <= flBestErr )
			{
				flBestErr = flErr;
				nBest = list.nAspect;
			}
		}
		return nBest;
	}

	// The size a shape switch lands on: the entry whose HEIGHT is closest to
	// the height already on screen, ties going to the wider one. Returns the
	// 1-based option value, so it is what SetSizeChoice() would have been
	// given by a click.
	//
	// Why height and not area or width: the user asked for it in exactly
	// those words ("make it automatically pick the closest resolution
	// (measured by height)"), and height is the axis that survives an aspect
	// change -- 1920x1080 -> 4:3 landing on 1440x1080 keeps the same vertical
	// detail, which is what "the same resolution, other shape" means to a
	// player. Ties go to the larger width for the same reason: of two equally
	// tall modes, the wider one shows more.
	inline int ClosestByHeight( const SizePreset *pPresets, size_t nPresets, int nHeight )
	{
		if ( !nPresets )
			return kSizeCustom;
		int nBest = 1;
		for ( size_t i = 1; i < nPresets; i++ )
		{
			const int nErr     = std::abs( pPresets[i].nHeight - nHeight );
			const int nBestErr = std::abs( pPresets[ nBest - 1 ].nHeight - nHeight );
			if ( nErr < nBestErr
			     || ( nErr == nBestErr && pPresets[i].nWidth > pPresets[ nBest - 1 ].nWidth ) )
				nBest = (int)i + 1;
		}
		return nBest;
	}

	inline int ClosestByHeight( const AspectList &list, int nHeight )
	{
		return ClosestByHeight( list.pSizes, list.nSizes, nHeight );
	}
}
