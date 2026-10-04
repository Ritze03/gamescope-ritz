#pragma once

// FrameGenHost.h -- gamescope-ritz's glue between the FrameGen library
// (subprojects/FrameGen/gpu/framegen.{h,cpp}, namespace `framegen`, compiled
// straight into this binary) and the compositor. Namespace `fghost`.
//
// WHAT THIS FILE IS: the whole public surface the other halves of frame
// generation code against --
//   * the settings panel (Overlay/)        : Config, SetConfig/GetConfig,
//                                            GetRenderStatus, GetPacingStatus
//   * the pacing logic (steamcompmgr side) : Enabled, SetSlot, Reset,
//                                            GetRenderStatus, PublishPacingStatus
//   * the display backends                 : Enabled (force a full composite)
//   * vulkan_composite() (rendervulkan.cpp): RenderWanted, RecordBaseLayer
//
// THE MODEL (superdoc/planning/fidelityfx-opticalflow-framegen.md 5.1/5.2).
// Real Nx: between two real game frames P (prev) and C (curr) the library
// makes N-1 generated frames at t = k/N, k = 1..N-1, and the compositor then
// shows C itself. Pacing decides WHICH of those a given display refresh shows
// and calls SetSlot(pairId, k, n) right before compositing it; the renderer
// here does the work lazily, once per slot (see RecordBaseLayer).
//
// OFF = ZERO COST. With multiplier 0 nothing in here runs per frame beyond one
// relaxed atomic load (RenderWanted()), no Interpolator exists, no texture is
// allocated, and the backends are free to direct-scanout. Turning it off frees
// everything (after a g_device.waitIdle()).
//
// THREADING. Config, slot, reset, pacing status and the status getters are safe
// from any thread (atomics / one small mutex). RecordBaseLayer() and the
// Vulkan state behind it belong to the one thread that runs vulkan_composite()
// (the library is not thread-safe and all records go on ONE queue in order).

#include <cstdint>

#include "rc.h"
#include "gamescope_shared.h"

class CVulkanTexture;

namespace fghost
{
	// ------------------------------------------------------------------
	//  User settings (decisions D1, D7)
	// ------------------------------------------------------------------

	// "Quality" preset. Quality = flow estimated at 1/2 resolution with
	// sub-pixel refinement (the library default). Performance = 1/4
	// resolution, no sub-pixel (~30-40 % cheaper, can miss thin fast detail).
	// The only STRUCTURAL setting: changing it does g_device.waitIdle() +
	// Interpolator::reconfigure() (a rare, user-driven stall).
	enum class Quality : uint8_t { Quality, Performance };

	// "Artifact safety": the per-pixel trust ramp (trustLow, trustHigh) in
	// 8-bit levels. Low = (16,56) smoother but more visible artefacts,
	// Default = (12,40), High = (8,28) more pixels fall back to the real
	// frame. Runtime (setSettings, no wait).
	enum class Safety : uint8_t { Low, Default, High };

	// "Static HUD protection": the zero-vector bonus. Off = 0, Normal = 1,
	// Strong = 2.5. Runtime.
	enum class HudProtect : uint8_t { Off, Normal, Strong };

	struct Config
	{
		// 0 = Off (the default). Otherwise 2, 3 or 4: N-1 generated frames per
		// real pair. SetConfig() normalises anything else (1 -> 0, > 4 -> 4).
		int multiplier = 0;
		Quality quality = Quality::Quality;
		Safety safety = Safety::Default;
		HudProtect hud = HudProtect::Normal;
	};

	// Replace the whole config. Thread-safe, cheap, callable at any time
	// (startup load, a user change, a profile switch). Takes effect at the next
	// vulkan_composite(): a preset change is applied live; a Quality change
	// additionally costs one waitIdle + reconfigure there. Setting the
	// multiplier to 0 releases every resource at the next composite (and asks
	// for one, so it does not wait for the next vblank that happens to draw).
	void   SetConfig( const Config &cfg );
	Config GetConfig();

	// The per-frame gate: multiplier >= 2. One relaxed atomic load. The three
	// backends OR this into their "needs full composite" decision (direct
	// scanout must be impossible while frame generation is on: a scanned-out
	// buffer never passes through vulkan_composite()), and steamcompmgr must
	// not pre-emptively upscale layer 0 while it is true (a pre-upscaled
	// texture reaches vulkan_composite() already baked with
	// bBaseLayerEffectsApplied, which is past the point FG can substitute).
	bool Enabled();

	// ------------------------------------------------------------------
	//  Pacing -> renderer
	// ------------------------------------------------------------------

	// Declare what the NEXT composite should show. Called by pacing from the
	// steamcompmgr thread before each composite.
	//   pairId : the commitID of the newest real frame, i.e. the frame
	//            composited as layer 0 (it is "curr" of the pair; the real
	//            frame before it is "prev"). A pairId not seen before is a NEW
	//            real frame: it is copied into the private ring even if this
	//            composite just shows it, so the next pair has a prev.
	//   k, n   : 1 <= k <= n-1 -> show the generated frame at t = k/n.
	//            k == 0 or k >= n -> show the real frame (pass-through).
	//            n < 2 (the initial state, before pacing ever calls this)
	//            -> the renderer does nothing at all, not even the ring copy.
	// Layer 0's texture MUST be the commit pairId names; the renderer cannot
	// check that. Cheap (one mutex-guarded copy of three words).
	void SetSlot( uint64_t pairId, int k, int n );

	// Drop the remembered previous frame and every cached generated frame:
	// the next frame composited has no prev (pass-through, then generation
	// resumes one real frame later). Call on a focus / size / format change or
	// a gap in the game's frames. Does not free anything. Thread-safe.
	void Reset();

	// ------------------------------------------------------------------
	//  Renderer status (decision D4, D16)
	// ------------------------------------------------------------------

	// Why the library cannot run (the real frame is shown instead). Ok while
	// it can, or while frame generation is off / pacing has not started.
	// NB: enumerators are Ok / Normal, not None: X11's X.h #defines None as 0L.
	enum class Unavailable : uint8_t
	{
		Ok,
		Hdr,          // HDR output enabled, or the game layer is HDR/scRGB/passthrough
		YCbCr,        // the game layer is a YCbCr (NV12) video surface
		NotEightBit,  // the game layer is not B8G8R8A8 / R8G8B8A8 (10-bit, 16-bit float, 565)
		TooSmall,     // below Interpolator::kMinSize (16) on an axis
		InitFailed,   // the library (or its textures) could not be created / resized
		RecordFailed, // a record call returned false for this frame
	};

	const char *UnavailableText( Unavailable eReason );

	struct RenderStatus
	{
		Unavailable reason = Unavailable::Ok;
		// GPU time of the last COMPLETED real pair: the motion estimate plus
		// every synth recorded for it, in milliseconds. < 0 = n/a (timestamps
		// unsupported on the compute queue, or nothing measured yet). Lags the
		// live pair by up to one pair: it is read only after that pair's
		// submissions have retired, never by waiting.
		float lastPairGpuMs = -1.0f;
		// g_device.supportsTimestamps(): false means lastPairGpuMs stays n/a.
		bool timestampsSupported = false;
	};

	RenderStatus GetRenderStatus();

	// ------------------------------------------------------------------
	//  Pacing status (published by steamcompmgr's pacing, read by the panel)
	// ------------------------------------------------------------------

	// Why pacing is not generating the full chosenN (or at all). The pacing
	// code owns the semantics and may append values; the panel maps them to
	// text via PassReasonText().
	enum class PassReason : uint8_t
	{
		Normal,         // generating the chosen multiplier
		Off,            // frame generation is off
		WarmingUp,      // no previous real frame yet
		RefreshLimit,   // the display cannot show N x the game's rate: stepped down
		CostGuard,      // measured generation cost would miss the display cadence: stepped down / pass-through
		GameTooFast,    // the game's own rate already fills the display
		GameStalled,    // a gap in the game's frames: showing real frames until it settles
		RendererUnavailable, // see RenderStatus::reason
	};

	const char *PassReasonText( PassReason eReason );

	struct PacingStatus
	{
		bool  valid = false;      // false until pacing has published once
		float gameFps = 0.0f;     // real frames per second the game produces
		float presentedFps = 0.0f;// frames per second actually shown (real + generated)
		int   chosenN = 0;        // the multiplier the user chose (0 = off)
		int   activeN = 0;        // the multiplier actually in use right now (<= chosenN)
		float delayMs = 0.0f;     // extra latency the hold-back costs, in milliseconds
		PassReason reason = PassReason::Normal; // why activeN < chosenN, or pass-through
	};

	// Mutex-protected snapshot, one writer (pacing), any reader (the panel).
	void         PublishPacingStatus( const PacingStatus &status );
	PacingStatus GetPacingStatus();

	// ------------------------------------------------------------------
	//  Renderer entry points -- vulkan_composite() ONLY
	// ------------------------------------------------------------------

	// True when vulkan_composite() should call RecordBaseLayer(): frame
	// generation is on, OR it was on and still holds resources that the next
	// call must release. This is the single atomic load the off path pays.
	bool RenderWanted();

	// Called once per full composite, BEFORE the ReShade / native-effects
	// passes, so everything downstream (ReShade, the effects pre-pass, zoom,
	// FSR/NIS, colour management, the overlays) sees the generated frame as
	// the frame (decision D3: FG runs on the RAW commit texture).
	//
	//   pLayer0     : the base layer's texture, or null when this composite is
	//                 not eligible (a partial composite, the pre-emptive upscale
	//                 pass, a layer already carrying the effects) -- null makes
	//                 this a no-op except that it still tears FG down if it was
	//                 switched off.
	//   eColorspace : that layer's colourspace (HDR is refused, decision D4).
	//
	// Returns the texture to use as layer 0 INSTEAD of pLayer0 (the caller
	// substitutes it in its private copy of the frame info), or null to show
	// pLayer0 unchanged. Never writes the caller's frame info.
	//
	// What it does, in its OWN command buffer submitted before returning (so
	// the caller's composite command buffer is untouched, and so a g_device.
	// waitIdle() here can never disturb it):
	//   * on the first sight of a new pairId: copies layer 0 into a private
	//     2-slot ring (decision D11 -- the game's buffers are never pinned);
	//   * for 1 <= k <= n-1: the first generated slot of a pair records
	//     recordEstimate + recordSynth(k/n), every later slot only its own
	//     recordSynth into a pooled output (decision D13); each output is cached
	//     by (pairId, k, n), so a repeat composite of the same slot (cursor
	//     move, overlay, screenshot) records nothing.
	gamescope::Rc<CVulkanTexture> RecordBaseLayer( gamescope::Rc<CVulkanTexture> pLayer0, GamescopeAppTextureColorspace eColorspace );
}
