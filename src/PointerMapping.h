// The absolute-pointer mapping: host (output-space) pointer -> focused
// surface (game-space) pointer, and the scaler ratios it is built from.
//
// Pure arithmetic, no globals, no wlroots -- so it is unit-testable
// (tests/test_pointer_mapping.cpp) the way CursorPolicy.h is. The live users:
//
//   - calc_scale_factor_scaler() (steamcompmgr.cpp) wraps ComputeScalerRatios()
//     with the globals (g_upscaleScaler, g_nNestedWidth/Height, the output
//     size, g_flMaxWindowScale). It decides how the base layer is drawn.
//   - paint_window_commit() turns those ratios into the base layer's
//     scale (1/ratio) and offset (-centering), which update_touch_scaling()
//     caches in focusedWindowScaleX/Y + focusedWindowOffsetX/Y.
//   - wlserver_touchmotion()/wlserver_touchdown() apply that cache to every
//     absolute pointer sample the nested backends deliver with force-grab off
//     (SDL windowed motion, the Wayland backend's wl_pointer.motion, real
//     touch). OutputToSurface() is that formula; SurfaceToOutput() is its
//     inverse, which nothing in the pipeline needs yet but the test uses to
//     pin the round trip.
//
// Why a copy of the layer transform and not the layer itself: the layer is
// rebuilt every frame from the *texture* size, and read from the input
// threads. Keeping the four numbers in a struct keeps the formula in one
// place and lets update_touch_scaling() compare old and new to know when the
// mapping moved (a runtime resolution change, a window resize, a fit override
// appearing) and re-sync the client's pointer -- see
// wlserver_resync_absolute_pointer().
#pragma once

#include <algorithm>
#include <cmath>

#include "main.hpp"

namespace gamescope
{
	struct ScalerRatios
	{
		float x = 1.0f;
		float y = 1.0f;
	};

	// Verbatim transcription of upstream's calc_scale_factor_scaler(), with
	// its globals as arguments. Returns the source -> output scale per axis.
	//
	// Under STRETCH the nested size cancels: (nested/src) * (out/nested) =
	// out/src per axis. Every other scaler keeps a single ratio for both axes
	// (letterbox/pillarbox) and does depend on the nested size through
	// outputScaleRatio.
	inline ScalerRatios ComputeScalerRatios(
		GamescopeUpscaleScaler eScaler,
		float flOutputWidth, float flOutputHeight,
		float flNestedWidth, float flNestedHeight,
		float flSourceWidth, float flSourceHeight,
		float flMaxWindowScale )
	{
		ScalerRatios r;

		const float XOutputRatio = flOutputWidth / flNestedWidth;
		const float YOutputRatio = flOutputHeight / flNestedHeight;
		const float outputScaleRatio = std::min( XOutputRatio, YOutputRatio );

		const float XRatio = flNestedWidth / flSourceWidth;
		const float YRatio = flNestedHeight / flSourceHeight;

		if ( eScaler == GamescopeUpscaleScaler::STRETCH )
		{
			r.x = XRatio * XOutputRatio;
			r.y = YRatio * YOutputRatio;
			return r;
		}

		if ( eScaler != GamescopeUpscaleScaler::FILL )
		{
			r.x = std::min( XRatio, YRatio );
			r.y = std::min( XRatio, YRatio );
		}
		else
		{
			r.x = std::max( XRatio, YRatio );
			r.y = std::max( XRatio, YRatio );
		}

		if ( eScaler == GamescopeUpscaleScaler::AUTO )
		{
			r.x = std::min( flMaxWindowScale, r.x );
			r.y = std::min( flMaxWindowScale, r.y );
		}

		r.x *= outputScaleRatio;
		r.y *= outputScaleRatio;

		if ( eScaler == GamescopeUpscaleScaler::INTEGER )
		{
			if ( r.x > 1.0f )
			{
				// x == y here always.
				r.x = r.y = std::floor( r.x );
			}
		}

		return r;
	}

	// The base layer's transform as the compositor stores it: surface pixel =
	// (output pixel + offset) * scale. scale is the *inverse* of the scaler
	// ratio, offset the negated centering, both per axis.
	struct AbsolutePointerMapping
	{
		double flScaleX = 1.0;
		double flScaleY = 1.0;
		double flOffsetX = 0.0;
		double flOffsetY = 0.0;

		bool operator==( const AbsolutePointerMapping & ) const = default;
	};

	// What paint_window_commit() produces for a base layer of flSourceWidth x
	// flSourceHeight drawn with the given ratios into an output of the given
	// size. The centering offset is truncated to whole pixels, exactly as the
	// compositor does (its drawXOffset/drawYOffset are ints).
	inline AbsolutePointerMapping MappingForBaseLayer(
		ScalerRatios r,
		int nOutputWidth, int nOutputHeight,
		int nSourceWidth, int nSourceHeight )
	{
		AbsolutePointerMapping m;
		const int nDrawXOffset = (int)( ( nOutputWidth - nSourceWidth * r.x ) / 2.0f );
		const int nDrawYOffset = (int)( ( nOutputHeight - nSourceHeight * r.y ) / 2.0f );
		m.flScaleX = 1.0 / r.x;
		m.flScaleY = 1.0 / r.y;
		m.flOffsetX = -nDrawXOffset;
		m.flOffsetY = -nDrawYOffset;
		return m;
	}

	// Output-space pixel -> surface-space pixel. No clamping: the caller
	// clamps to the surface's own bounds, which are not part of the mapping.
	inline void OutputToSurface( const AbsolutePointerMapping &m, double flOutX, double flOutY, double *pflSurfX, double *pflSurfY )
	{
		*pflSurfX = ( flOutX + m.flOffsetX ) * m.flScaleX;
		*pflSurfY = ( flOutY + m.flOffsetY ) * m.flScaleY;
	}

	// The inverse: surface-space pixel -> output-space pixel.
	inline void SurfaceToOutput( const AbsolutePointerMapping &m, double flSurfX, double flSurfY, double *pflOutX, double *pflOutY )
	{
		*pflOutX = flSurfX / m.flScaleX - m.flOffsetX;
		*pflOutY = flSurfY / m.flScaleY - m.flOffsetY;
	}
}
