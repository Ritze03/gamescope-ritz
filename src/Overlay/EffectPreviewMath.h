#pragma once

// EffectPreviewMath.h -- the pure half of the Inspector's Adaptive Brightness
// before/after strip: everything that turns a captured frame plus a set of
// slider values into the RGBA pixels the strip shows. Header-only and free of
// ImGui, Vulkan and every overlay global, exactly like CrosshairMath.h, so
// tests/test_overlay_ui.cpp can exercise it without a GPU or a context.
//
// THE ONE THING THAT MAKES THE RIGHT HALF TRUSTWORTHY: it applies
// src/shaders/effects_curve.h -- the same text the GPU compiles -- to the
// same statistics the GPU measured for the captured frame (they are copied
// out of the history texture with the pixels, see rendervulkan.cpp's
// effects_preview_flush()). So the "after" half is not an approximation of
// the effect; it is the effect, evaluated on the CPU.
//
// WHAT IT DELIBERATELY DOES NOT MODEL. Pre-Sharpen (a 5-tap cross at source
// resolution -- invisible after the ~7x downscale, and it does not move the
// statistics either). Shadow Control, Saturation and Vibrancy are already
// baked into the captured pixels by cs_effects_preview.comp's grade() call,
// so both halves carry them and the strip isolates Adaptive Brightness
// alone -- which is what it is for.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "shaders/effects_curve.h"

namespace gamescope::overlay::abpreview
{
	// The statistics a captured frame carries: the smoothed values the GPU's
	// per-pixel pass read for that frame, plus Local adaptation's 16x16 map.
	struct Stats
	{
		float flMean = 0.0f;
		float flP2   = 0.0f;
		float flP50  = 0.0f;
		float flP98  = 0.0f;
		const float *pflLocal = nullptr;   // nGrid * nGrid, row-major; may be null
		int   nGrid  = 16;
	};

	// The slider positions, straight from the panel. Names match
	// NativeEffectsState_t's, deliberately -- this is the same parameter set.
	struct Params
	{
		bool  bDynamic   = false;
		float flTarget   = 0.5f;
		float flMinGain  = 0.3f;
		float flMaxGain  = 4.0f;
		float flStrength = 1.0f;
		float flLocal    = 0.0f;
	};

	// The local map, sampled bilinearly at normalised image position (u, v).
	// A line-for-line port of effects_common.h's ab_local_sample(), including
	// its clamp-to-edge at the border: the half-cell outside the outermost
	// cell centres reads that cell.
	inline float LocalSample( const float *pflMap, int nGrid, float u, float v )
	{
		if ( !pflMap || nGrid <= 0 )
			return 0.0f;
		const auto Cl = []( float x, float lo, float hi ) { return std::min( std::max( x, lo ), hi ); };
		const float gx = Cl( Cl( u, 0.0f, 1.0f ) * (float)nGrid - 0.5f, 0.0f, (float)( nGrid - 1 ) );
		const float gy = Cl( Cl( v, 0.0f, 1.0f ) * (float)nGrid - 0.5f, 0.0f, (float)( nGrid - 1 ) );
		const int x0 = (int)gx, y0 = (int)gy;
		const int x1 = std::min( x0 + 1, nGrid - 1 ), y1 = std::min( y0 + 1, nGrid - 1 );
		const float fx = gx - (float)x0, fy = gy - (float)y0;
		const float a = pflMap[ y0 * nGrid + x0 ];
		const float b = pflMap[ y0 * nGrid + x1 ];
		const float c = pflMap[ y1 * nGrid + x0 ];
		const float d = pflMap[ y1 * nGrid + x1 ];
		return ( a + ( b - a ) * fx ) + ( ( c + ( d - c ) * fx ) - ( a + ( b - a ) * fx ) ) * fy;
	}

	// Adaptive Brightness applied to one encoded RGB triplet at normalised
	// image position (u, v). A port of cs_effects_layer0.comp's Adaptive
	// Brightness block, both modes, in the same order and with the same
	// clamps. Values in and out are 0..1 encoded.
	inline void ApplyPixel( float u, float v, const Stats &st, const Params &p, float flRgb[3] )
	{
		const auto Cl = []( float x, float lo, float hi ) { return std::min( std::max( x, lo ), hi ); };
		const float flStrength = Cl( p.flStrength, 0.0f, 1.0f );

		namespace ec = gamescope::effects_curve;
		if ( p.bDynamic )
		{
			float flREff = 1.0f;
			if ( p.flLocal > 0.0f && st.pflLocal )
				flREff = ec::ab_local_shift( LocalSample( st.pflLocal, st.nGrid, u, v ), st.flMean, p.flLocal );
			const float flGain  = ec::ab_dyn_gain( st.flP98 * flREff, st.flP50 * flREff,
			                                       p.flTarget, p.flMinGain, p.flMaxGain );
			const float flGamma = ec::ab_dyn_gamma( st.flP2 * flREff, st.flP50 * flREff,
			                                        flGain, p.flTarget, p.flMinGain, p.flMaxGain );
			for ( int i = 0; i < 3; i++ )
			{
				const float flGraded = ec::ab_dyn_curve( flRgb[i], flGain, flGamma );
				flRgb[i] = Cl( flRgb[i] + ( flGraded - flRgb[i] ) * flStrength, 0.0f, 1.0f );
			}
		}
		else
		{
			const float flGain = Cl( p.flTarget / std::max( st.flMean, 0.001f ), p.flMinGain, p.flMaxGain );
			for ( int i = 0; i < 3; i++ )
			{
				const float flGraded = flRgb[i] * flGain;
				flRgb[i] = Cl( flRgb[i] + ( flGraded - flRgb[i] ) * flStrength, 0.0f, 1.0f );
			}
		}
	}

	// Where the split falls, in texture columns. Left of it is untouched,
	// right of it is graded. Its own function because the strip's divider,
	// the composed texture and the tests all have to agree on one number.
	inline int SplitColumn( int nWidth ) { return nWidth / 2; }

	// True when the effect is a pure per-byte function of the input, i.e.
	// when its gain and gamma are frame constants: Whole image always, and
	// Dynamic while Local adaptation is off. Compose() then evaluates the
	// curve 256 times instead of 110,592 times -- see its comment.
	inline bool IsUniform( const Stats &st, const Params &p )
	{
		return !p.bDynamic || p.flLocal <= 0.0f || st.pflLocal == nullptr;
	}

	// Compose the strip's texture: an RGBA32 image of the captured frame with
	// Adaptive Brightness applied to the RIGHT half only. `pSrcRgb` is
	// nWidth * nHeight * 3 tightly packed bytes; `pDstRgba` is
	// nWidth * nHeight * 4.
	//
	// A single frozen frame, split down the middle -- NOT two different
	// crops. Both halves are the same photograph, so a feature that straddles
	// the divider shows its own before and after touching each other.
	inline void Compose( const uint8_t *pSrcRgb, int nWidth, int nHeight,
	                     const Stats &st, const Params &p, uint8_t *pDstRgba )
	{
		if ( !pSrcRgb || !pDstRgba || nWidth <= 0 || nHeight <= 0 )
			return;
		const int nSplit = SplitColumn( nWidth );

		// THE 256-ENTRY TABLE, AND WHY IT IS NOT AN APPROXIMATION. Whenever
		// the gain and the gamma are frame constants (IsUniform() above), the
		// whole effect is a pure function of one byte -- the three channels
		// are independent and take the same curve -- and the input has 256
		// possible values. So the table is built by calling the SAME
		// ApplyPixel() on each of them: one definition of the maths, 256
		// evaluations instead of nWidth * nHeight * 3, and a result that is
		// equal by construction rather than close. Measured on this desktop
		// at 256x144: 2150 us -> 45 us in Whole image / Dynamic-without-local.
		// Dynamic WITH Local adaptation cannot use it -- every pixel gets its
		// own gain -- and pays the full ~2.1 ms; see shader-effects.md.
		const bool bUniform = IsUniform( st, p );
		uint8_t lut[ 256 ];
		if ( bUniform )
		{
			for ( int v = 0; v < 256; v++ )
			{
				float flRgb[3] = { (float)v / 255.0f, (float)v / 255.0f, (float)v / 255.0f };
				ApplyPixel( 0.5f, 0.5f, st, p, flRgb );
				lut[v] = (uint8_t)std::lround( std::min( std::max( flRgb[0], 0.0f ), 1.0f ) * 255.0f );
			}
		}

		for ( int y = 0; y < nHeight; y++ )
		{
			const float v = ( (float)y + 0.5f ) / (float)nHeight;
			for ( int x = 0; x < nWidth; x++ )
			{
				const uint8_t *pS = pSrcRgb + ( (size_t)y * nWidth + x ) * 3;
				uint8_t *pD = pDstRgba + ( (size_t)y * nWidth + x ) * 4;
				pD[3] = 255;
				if ( x < nSplit )
				{
					pD[0] = pS[0]; pD[1] = pS[1]; pD[2] = pS[2];
					continue;
				}
				if ( bUniform )
				{
					pD[0] = lut[ pS[0] ]; pD[1] = lut[ pS[1] ]; pD[2] = lut[ pS[2] ];
					continue;
				}
				float flRgb[3] = { pS[0] / 255.0f, pS[1] / 255.0f, pS[2] / 255.0f };
				ApplyPixel( ( (float)x + 0.5f ) / (float)nWidth, v, st, p, flRgb );
				for ( int i = 0; i < 3; i++ )
					pD[i] = (uint8_t)std::lround( std::min( std::max( flRgb[i], 0.0f ), 1.0f ) * 255.0f );
			}
		}
	}
}
