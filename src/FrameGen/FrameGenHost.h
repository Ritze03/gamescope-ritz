#pragma once

// FrameGenHost.h -- gamescope-ritz's glue between the FrameGen library
// (subprojects/FrameGen/gpu/framegen.{h,cpp}, namespace `framegen`, compiled
// straight into this binary) and the compositor. Namespace `fghost`.
//
// WHAT THIS FILE IS: the whole public surface the other halves of frame
// generation code against --
//   * the settings panel (Overlay/)        : Config, SetConfig/GetConfig,
//                                            GetRenderStatus, GetPacingStatus
//   * the pacing logic (steamcompmgr side) : Enabled, SetFrame, Reset,
//                                            GetRenderStatus, PublishPacingStatus
//   * the display backends                 : Enabled (force a full composite)
//   * vulkan_composite() (rendervulkan.cpp): RenderWanted, RecordBaseLayer
//
// THE MODEL (superdoc/features/frame-generation.md, "Pacing"). Between two real
// game frames P (prev) and C (curr) the library can make a generated frame at
// ANY t in (0,1); pacing (FrameGen/Pacing.h) decides, for every output frame
// (every vblank that is due a new image), which t to show, and calls
// SetFrame(...) right before compositing it. The renderer does the work lazily,
// once per output frame (see RecordBaseLayer). There is no "k of N" any more:
// one estimate per pair, any number of synths at any t.
//
// OFF = ZERO COST. With the mode Off nothing in here runs per frame beyond one
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

	// What the user asked for. Off = nothing runs. Fixed = a multiplier of the
	// game's rate (output saturates at the refresh rate, it never steps down).
	// Target = aim at an output frame rate whatever the game does.
	enum class Mode : uint8_t { Off, Fixed, Target };

	// What pacing trades when the game's frame times jitter. LowLatency adds the
	// least delay (the content time snaps to each new real frame, so uneven
	// frame times show as uneven motion). Smoothness spaces the content evenly
	// and queues about one game frame.
	enum class Priority : uint8_t { LowLatency, Smoothness };

	// The largest fixed multiplier.
	constexpr int kMaxMultiplier = 8;

	struct Config
	{
		Mode mode = Mode::Off;
		// Fixed: 2..kMaxMultiplier. SetConfig() normalises (< 2 with mode Fixed
		// -> mode Off, > kMaxMultiplier -> kMaxMultiplier). Kept as chosen in the
		// other modes (0 if never set).
		int multiplier = 0;
		// Target: the output fps to aim for, 30..1000; 0 = the display's refresh.
		int targetFps = 0;
		Priority priority = Priority::LowLatency;
		Quality quality = Quality::Quality;
		Safety safety = Safety::Default;
		HudProtect hud = HudProtect::Normal;
	};

	// Replace the whole config. Thread-safe, cheap, callable at any time
	// (startup load, a user change, a profile switch). Takes effect at the next
	// vulkan_composite(): a preset change is applied live; a Quality change
	// additionally costs one waitIdle + reconfigure there. Setting the
	// mode to Off releases every resource at the next composite (and asks
	// for one, so it does not wait for the next vblank that happens to draw).
	void   SetConfig( const Config &cfg );
	Config GetConfig();

	// The per-frame gate: mode != Off. One relaxed atomic load. The three
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

	// What the NEXT composite should show: one output frame.
	struct FrameRequest
	{
		// The commit that is layer 0 (the newest real frame). A newestId not seen
		// before is a NEW real frame: it is copied into the private ring even if
		// this composite does not show it, so later pairs have it. 0 = inert.
		uint64_t newestId = 0;
		// The real pair to synthesise from, by commit id (prevId before currId).
		// Smoothness may play a pair one older than the newest; both must still be
		// in the ring (the last three real frames) or the real frame is shown.
		uint64_t prevId = 0;
		uint64_t currId = 0;
		// The output frame's identity. The SAME outId is the SAME image, so a
		// repaint of it (cursor move, overlay, screenshot) reuses the cached
		// output and records nothing. A new outId needs a new synth.
		uint64_t outId = 0;
		// t < 0   : INERT -- the renderer does nothing at all, not even the ring
		//           copy (settled pass-through; the cost with FG on but unused).
		// t >= 1  : the real frame newestId (layer 0). The ring copy still
		//           happens, so the next pair has its frame.
		// 0 < t < 1: a generated frame between prevId and currId at t.
		float t = -1.0f;
	};

	// Called by pacing from the steamcompmgr thread before each composite.
	// Layer 0's texture MUST be the commit newestId names; the renderer cannot
	// check that. Cheap (one mutex-guarded copy of a few words).
	void SetFrame( const FrameRequest &request );

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
		// The same measurement split: the motion estimate (once per real pair) and
		// ONE synth (once per generated frame), milliseconds, < 0 = n/a. Cost of a
		// pair with g generated frames is estimate + g x synth; the pacing cost
		// guard budgets from these. lastSynthMs is the mean over the synths the
		// profile covered (the library's query pool holds about a dozen).
		float lastEstimateMs = -1.0f;
		float lastSynthMs = -1.0f;
		// Increments with every new measurement, so pacing can tell a fresh
		// reading from the stale one it already acted on.
		uint32_t costSeq = 0;
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
		Normal,         // generating as asked
		Off,            // frame generation is off
		WarmingUp,      // no previous real frame yet
		CostGuard,      // measured generation cost would miss the budget: output rate lowered / pass-through
		GameTooFast,    // the game's own rate already reaches the output rate (refresh or target)
		GameStalled,    // a gap in the game's frames: showing real frames until it settles
		RendererUnavailable, // see RenderStatus::reason
	};

	const char *PassReasonText( PassReason eReason );

	struct PacingStatus
	{
		bool  valid = false;      // false until pacing has published once
		float gameFps = 0.0f;     // real frames per second the game produces
		// Output frames actually presented per second: generated frames plus the
		// real frames pacing decided to show. A repaint of an already-shown output
		// (cursor, overlay) is NOT counted.
		float presentedFps = 0.0f;
		float effectiveMultiplier = 0.0f; // presentedFps / gameFps while generating, else 0
		float targetFps = 0.0f;   // Target mode: the rate aimed for (clamped to the refresh); else 0
		int   chosenN = 0;        // Fixed mode: the multiplier the user chose; Target mode: 0
		// round(effectiveMultiplier), at least 2 while generating, 0 while passing
		// real frames through. "activeN >= 2" therefore means "generating" -- the
		// HUD's Count generated frames option relies on that.
		int   activeN = 0;
		float delayMs = 0.0f;     // D: the extra latency pacing adds, in milliseconds
		PassReason reason = PassReason::Normal; // why it is not generating as asked, or pass-through
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
	//   * on the first sight of a new newestId: copies layer 0 into a private
	//     3-slot ring (decision D11 -- the game's buffers are never pinned) and
	//     runs crosshair detect/inpaint on it;
	//   * for 0 < t < 1: the first synth of a real pair records recordEstimate
	//     first, then recordSynth(t) into a pooled output (decision D13) cached
	//     by outId, so a repeat composite of the same output records nothing.
	gamescope::Rc<CVulkanTexture> RecordBaseLayer( gamescope::Rc<CVulkanTexture> pLayer0, GamescopeAppTextureColorspace eColorspace );
}
