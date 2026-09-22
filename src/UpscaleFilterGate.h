#pragma once

// Whether the FSR/NIS compute passes should run for a base layer with the
// given per-axis scale (texture size / destination size -- <1 is an
// upscale, ==1 is exact native resolution, >1 is a downscale; see
// FrameInfo_t::Layer_t::scale in rendervulkan.hpp).
//
// Runs at native resolution too, as a sharpen-only pass (2026-09-22, the
// user: "make sure that FreeSync and NIST upscaling works even at the
// native resolution, so it behaves more like a filter instead of an actual
// upscaler" -- "FreeSync and NIST" is speech-to-text for FSR and NIS). At
// exactly 1.0 EASU/NIS dispatch input->input (a near-identity resample) and
// RCAS/NIS's own sharpen term still applies on top, at the configured
// Sharpness strength.
//
// Still excluded on downscale (either axis > 1 + epsilon): EASU's
// documented range is 1x-4x UPsampling only (ffx_fsr1.h), and NIS's own
// NVScalerUpdateConfig() rejects kScaleX/kScaleY > 1 (NIS_Config.h) --
// neither filter is designed to run backwards. See
// superdoc/features/scaling-filters.md's "FSR/NIS apply at native
// resolution too".
inline bool FilterPassApplies( float flScaleX, float flScaleY )
{
	constexpr float kEpsilon = 0.001f;
	return flScaleX <= 1.0f + kEpsilon && flScaleY <= 1.0f + kEpsilon;
}
