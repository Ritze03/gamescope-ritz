#pragma once

// EffectPreviewMath.h -- the pure half of the Inspector's before/after strip
// for whichever adaptive effect is on (Adaptive Brightness, or -- since
// 2026-09-08 -- Adaptive Gamma; they are mutually exclusive, so the strip
// never has to show two things at once): everything that turns a captured
// frame plus a set of slider values into the RGBA pixels the strip shows. Header-only and free of
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
// so both halves carry them and the strip isolates the adaptive effect
// alone -- which is what it is for.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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
		// Adaptive Brightness V2's own content-median anchor (NEW
		// 2026-09-14) -- filled by V2AnchorFromPixels() above, NOT by the
		// GPU capture (this effect's anchor is not threaded through the
		// history readback the other four statistics are).
		float flV2Anchor = 0.5f;
	};

	// The slider positions, straight from the panel. Names match
	// NativeEffectsState_t's, deliberately -- this is the same parameter set.
	//
	// ONE STRUCT FOR BOTH ADAPTIVE EFFECTS (Adaptive Gamma, 2026-09-08).
	// They are mutually exclusive, so only one of them can ever be the one
	// the strip is showing; `bGamma` says which. The three fields that mean
	// the same thing in both -- Target, Strength, Local adaptation -- are
	// shared rather than duplicated with an `Ag` prefix; the bounds are not
	// shared, because a gain bound and an exponent bound are different
	// numbers with different units. The caller fills whichever set applies.
	struct Params
	{
		bool  bGamma     = false;   // Adaptive Gamma instead of Adaptive Brightness
		bool  bDynamic   = false;   // Adaptive Brightness's Dynamic mode
		float flTarget   = 0.5f;
		float flMinGain  = 0.3f;    // Adaptive Brightness only
		float flMaxGain  = 4.0f;    // Adaptive Brightness only
		float flStrength = 1.0f;
		float flLocal    = 0.0f;
		float flMaxLift   = 4.0f;   // Adaptive Gamma only: exponent floor = 1/this
		float flMaxDarken = 1.5f;   // Adaptive Gamma only: exponent ceiling

		// Adaptive Brightness V2 (NEW 2026-09-14) -- a THIRD choice, mutually
		// exclusive with the two above (bGamma/bDynamic are then both
		// false). flTarget/flStrength are NOT reused here: V2 has no
		// dry/wet mix (dropped per the plan -- Lift is the always-on
		// strength) and its own Target is a separate field below, because
		// it means something different (the CONTENT median, not the whole
		// frame's).
		bool  bV2       = false;
		bool  bV2Scene  = true;    // Adaptation: Off vs Scene
		bool  bV2Knee   = false;   // Shape: toe vs knee
		float flV2Lift    = 0.5f;
		float flV2Target  = 0.35f;
		float flV2MaxLift = 4.0f;
		// DARKENING (NEW 2026-09-14) -- the mirror pair; see effects_curve.h.
		float flV2MaxDarken = 1.0f;
		float flV2Darken    = 0.0f;
	};

	// Adaptive Brightness V2's own content-median anchor (plan 4.4), computed
	// directly from the CAPTURED strip's pixels rather than from a GPU-side
	// history texel -- a BASE-ONLY approximation, same as ApplyPixel()'s own
	// v2 branch below (no guided filter on the CPU preview; see that
	// function's comment). Good enough to judge a slider by on a
	// 256x144 strip, not a substitute for the GPU's own measure pass.
	inline float V2AnchorFromPixels( const uint8_t *pRgb, int nWidth, int nHeight, bool *pbVoid = nullptr )
	{
		if ( pbVoid ) *pbVoid = false;
		if ( !pRgb || nWidth <= 0 || nHeight <= 0 )
			return 0.5f;
		// A plain sorted-vector rank window (this runs once per Inspector
		// open/param change, not per pixel) -- 256*144 luma samples is a
		// trivial sort at this cadence.
		std::vector<float> vContent;
		vContent.reserve( (size_t)nWidth * nHeight );
		for ( int i = 0; i < nWidth * nHeight; i++ )
		{
			const uint8_t *p = pRgb + (size_t)i * 3;
			const float luma = ( 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] ) / 255.0f;
			if ( luma > gamescope::effects_curve::ABV2_BLACK )
				vContent.push_back( luma );
		}
		if ( vContent.size() < (size_t)( 0.01 * nWidth * nHeight ) )
		{
			if ( pbVoid ) *pbVoid = true;
			return 0.5f;   // "void" -- see the shader's own handling
		}
		std::sort( vContent.begin(), vContent.end() );
		const size_t n = vContent.size();
		const size_t lo = (size_t)( 0.25 * n ), hi = (size_t)( 0.75 * n );
		double total = 0.0;
		size_t taps = 0;
		for ( size_t i = lo; i < std::max( hi, lo + 1 ); i++ )
		{
			if ( i >= n ) break;
			total += vContent[i];
			taps++;
		}
		return taps > 0 ? (float)std::clamp( total / (double)taps, 0.0, 1.0 ) : 0.5f;
	}

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

	// The adaptive effect applied to one encoded RGB triplet at normalised
	// image position (u, v). A port of cs_effects_layer0.comp's Adaptive
	// Brightness block (both modes) and its Adaptive Gamma block, in the same
	// order and with the same clamps. Values in and out are 0..1 encoded.
	inline void ApplyPixel( float u, float v, const Stats &st, const Params &p, float flRgb[3] )
	{
		const auto Cl = []( float x, float lo, float hi ) { return std::min( std::max( x, lo ), hi ); };
		const float flStrength = Cl( p.flStrength, 0.0f, 1.0f );

		namespace ec = gamescope::effects_curve;
		if ( p.bV2 )
		{
			// BASE-ONLY APPROXIMATION (Stage 2's guided filter is not
			// modelled here -- B == Y, D == 0 always, i.e. exactly what
			// the shader itself falls back to for a void pixel). Good
			// enough to judge Shape/Target/Lift/Max lift by; Detail has no
			// visible effect in this strip, which is the one thing this
			// approximation cannot show -- said so in this file's header.
			const float Y = std::clamp( 0.299f * flRgb[0] + 0.587f * flRgb[1] + 0.114f * flRgb[2], 0.0f, 1.0f );
			if ( ec::abv2_is_void( Y ) )
				return;
			// V2 darken QC (2026-09-15 S-curve redesign): the lift half's own
			// aim must be abv2_g_lift_scurve(), not plain abv2_g() -- the GPU
			// (cs_effects_layer0.comp) already calls the former once Max
			// darken is active, because the rescaled curve's domain on this
			// half is x/Target, not raw x (see that function's own comment
			// in effects_curve.h). Delegates to abv2_g() byte-for-byte
			// whenever Max darken is at its floor, so this is a strict
			// superset of the old call, not a behaviour change at the
			// shipped default (Max darken 1).
			const float g = ec::abv2_g_lift_scurve( p.flV2Lift, p.flV2Target, st.flV2Anchor,
			                                          p.bV2Scene, p.flV2MaxDarken );
			// DARKENING (NEW 2026-09-14): the same two-sided curve the GPU
			// runs (effects_curve.h's abv2_curve2()) -- see that block for
			// the pivot construction. Byte-identical to the lift-only line
			// above at the darkening params' own defaults (Max darken 1,
			// Darken 0).
			const float gDark = ec::abv2_g_dark( p.flV2Darken, p.flV2Target, st.flV2Anchor, p.bV2Scene );
			const float Yp = ec::abv2_curve2( Y, g, p.flV2MaxLift, p.bV2Knee,
			                                   gDark, p.flV2MaxDarken, p.flV2Target );
			const float k = Yp / std::max( Y, 1e-4f );
			for ( int i = 0; i < 3; i++ )
				flRgb[i] = Cl( flRgb[i] * k, 0.0f, 1.0f );
			return;
		}
		if ( p.bGamma )
		{
			// Adaptive Gamma: a port of cs_effects_layer0.comp's step 5, in
			// the same order and with the same clamps -- one exponent from
			// the (optionally locally-shifted) median, no gain and no
			// shoulder to model because the effect has neither.
			float flREff = 1.0f;
			if ( p.flLocal > 0.0f && st.pflLocal )
				flREff = ec::ab_local_shift( LocalSample( st.pflLocal, st.nGrid, u, v ), st.flMean, p.flLocal );
			const float flGamma = ec::ag_gamma( st.flP50 * flREff, p.flTarget, p.flMaxLift, p.flMaxDarken );
			for ( int i = 0; i < 3; i++ )
			{
				const float flGraded = ec::ag_curve( flRgb[i], flGamma );
				flRgb[i] = Cl( flRgb[i] + ( flGraded - flRgb[i] ) * flStrength, 0.0f, 1.0f );
			}
		}
		else if ( p.bDynamic )
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
	// Dynamic (or Adaptive Gamma) while Local adaptation is off. Compose()
	// then evaluates the curve 256 times instead of 110,592 times -- see its
	// comment.
	inline bool IsUniform( const Stats &st, const Params &p )
	{
		// Adaptive Brightness V2's base-only approximation has NO spatial
		// dependency at all (B == Y everywhere, no guided filter modelled),
		// so it is uniform unconditionally -- unlike the other two, it has
		// no per-pixel branch to ask about.
		if ( p.bV2 )
			return true;
		const bool bPerPixel = ( p.bGamma || p.bDynamic )
			&& p.flLocal > 0.0f && st.pflLocal != nullptr;
		return !bPerPixel;
	}

	// Compose the strip's texture: an RGBA32 image of the captured frame with
	// the adaptive effect applied to the RIGHT half only. `pSrcRgb` is
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
