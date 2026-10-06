#pragma once

// FrameGenHost.h -- gamescope-ritz's glue between the FrameGen library
// (subprojects/FrameGen/gpu/framegen.{h,cpp}, namespace `framegen`, compiled
// straight into this binary) and the compositor. Namespace `fghost`.
//
// WHAT THIS FILE IS: the whole public surface the other halves of frame
// generation code against --
//   * the settings panel (Overlay/)        : Config, SetConfig/GetConfig,
//                                            GetRenderStatus, GetPacingStatus
//   * the pacing glue (steamcompmgr side)  : Active, SetFrame, Reset,
//                                            GetRenderStatus, PublishPacingStatus
//   * the display backends                 : Enabled (force a full composite)
//   * vulkan_composite() (rendervulkan.cpp): RenderWanted, RecordBaseLayer
//
// THE MODEL (superdoc/features/frame-generation.md, "Pacing"). Between two real
// game frames P (prev) and C (curr) the library can make a generated frame at
// ANY t in (0,1); pacing (frame-gen-ritz's gpu/pacing.h, framegen::pacing --
// the library owns it; steamcompmgr only feeds it and applies its Decision)
// decides, for every output frame (every vblank that is due a new image), which
// pair, which t / blur window [t0, t1] or which real frame to show, and calls
// SetFrame(...) right before compositing it. MOTION BLUR (BlurConfig) is the
// same renderer call with a window instead of a point: recordSynthBlur(). The renderer does the work lazily,
// once per output frame (see RecordBaseLayer). There is no "k of N" any more:
// one estimate per pair, any number of synths at any t.
//
// OFF = ZERO COST. With frame generation, motion blur AND the lag spike buffer off nothing in here runs per frame beyond one
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
	// frame. Off (last, so the packed values of the others stay put) disables every
	// fallback the library has: trust ramp (254,255), globalFallback 1.0 and
	// sceneCutSad 255 -- never the real frame, even on fast flicks and scene cuts.
	// Runtime (setSettings, no wait).
	enum class Safety : uint8_t { Low, Default, High, Off };

	// "Static HUD protection": the zero-vector bonus. Off = 0, Normal = 1,
	// Strong = 2.5. Runtime.
	enum class HudProtect : uint8_t { Off, Normal, Strong };

	// "UI protection": the FrameGen library's static-UI protection
	// (framegen::UiProtection), independent of HudProtect. Crosshair = a centred
	// box (Config::uiBoxTenths of the frame's height), WholeScreen = every solid
	// still HUD element too, CrosshairV2 = the box again with a different
	// detector (only still, contrasty pixels; no fill-in). The values match the
	// library's enum. Runtime (setSettings, no wait). Stored in the packed config
	// at bits 24-25, so Off = 0 would be the all-zero word; the DEFAULT is Crosshair.
	enum class UiProt : uint8_t { Off, Crosshair, WholeScreen, CrosshairV2 };

	// The crosshair box's side, in tenths of a percent of the game's height
	// (25 = 2.5%). The library's side = clamp(round(height * frac), 8, min(w, h)).
	constexpr int kUiBoxMinTenths = 5;
	constexpr int kUiBoxMaxTenths = 100;
	constexpr int kUiBoxDefaultTenths = 25;

	// What the user asked for. Off = nothing runs. Fixed = a multiplier of the
	// game's rate (output saturates at the refresh rate, it never steps down).
	// Target = aim at an output frame rate whatever the game does.
	enum class Mode : uint8_t { Off, Fixed, Target };

	// The "Pause at refresh rate" value the pacer actually gets: the stored switch
	// only means something for the fixed multipliers. In Target mode it is ignored
	// (the target stands as set), whatever was stored. Why: the user -- "it
	// shouldn't be affected by the slider as well, it should then just have no
	// effect". Pack() applies it, so every reader of GetConfig() sees the result.
	constexpr bool EffectiveCapAtRefresh( bool bPause, Mode eMode ) { return bPause && eMode != Mode::Target; }

	// What pacing trades when the game's frame times jitter. LowLatency adds the
	// least delay (the content time snaps to each new real frame, so uneven
	// frame times show as uneven motion). Smoothness spaces the content evenly
	// and queues about one game frame.
	enum class Priority : uint8_t { LowLatency, Smoothness };

	// The largest fixed multiplier.
	constexpr int kMaxMultiplier = 8;

	// ------------------------------------------------------------------
	//  Motion blur (the Motion blur settings area; the library does the work)
	// ------------------------------------------------------------------

	// What the shutter is measured against (framegen::pacing::BlurRelative):
	// Shown = one SHOWN-frame interval (subtle), Game = one GAME-frame interval
	// (the "film look").
	enum class BlurRel : uint8_t { Shown, Game };

	// Weights of the sub-frames (framegen::BlurWeights).
	enum class BlurWeighting : uint8_t { Even, Gaussian };

	constexpr int kMinBlurSamples = 2;
	constexpr int kMaxBlurSamples = 8;   // the library takes 16; the UI offers 2..8

	// Independent of Config: motion blur works with frame generation off, and
	// both on at once. Pushed to the renderer (library settings) and the pacer
	// (Inputs) live; nothing here waits.
	struct BlurConfig
	{
		bool enabled = false;
		int samples = 4;             // kMinBlurSamples..kMaxBlurSamples (SetBlurConfig clamps)
		int amountPercent = 50;      // the shutter, 0..100
		BlurRel relative = BlurRel::Shown;
		BlurWeighting weights = BlurWeighting::Gaussian;
	};

	// Replace the whole blur config. Thread-safe, cheap, any time. Like
	// SetConfig(): switching the last feature off asks for one more composite so
	// the resources are released.
	void       SetBlurConfig( const BlurConfig &cfg );
	BlurConfig GetBlurConfig();

	// ------------------------------------------------------------------
	//  Lag spike buffer (the Lag spike buffer settings area; the library does the work)
	// ------------------------------------------------------------------

	// framegen::pacing::LagTestMode: Off = size from the game's own spikes, ForceMin
	// pins the buffer's target at 0, ForceMax at maxBufferMs (so the user can feel
	// both ends).
	enum class LagTest : uint8_t { Off, ForceMin, ForceMax };

	constexpr int kMinLagLookbackMin = 1;
	constexpr int kMaxLagLookbackMin = 10;   // the library takes 600 s; the UI offers 1..10 minutes
	constexpr int kMaxLagBufferMs = 250;

	// Independent of Config AND BlurConfig: the buffer fills gaps with generated
	// frames whether or not frame generation (the multiplier) is on, and combines
	// with blur. Pushed to the pacer (Inputs::lagBuffer) live; nothing here waits.
	struct LagBufferConfig
	{
		bool enabled = false;
		int lookbackMin = 5;          // kMinLagLookbackMin..kMaxLagLookbackMin (SetLagBufferConfig clamps)
		int maxBufferMs = 50;         // 0..kMaxLagBufferMs
		LagTest testMode = LagTest::Off;
	};

	// Replace the whole lag spike buffer config. Thread-safe, cheap, any time. Like
	// SetConfig(): switching the last feature off asks for one more composite so the
	// resources are released.
	void           SetLagBufferConfig( const LagBufferConfig &cfg );
	LagBufferConfig GetLagBufferConfig();

	struct Config
	{
		// The master switch. false: nothing runs (mode / multiplier are kept).
		bool enabled = false;
		Mode mode = Mode::Target;
		// Fixed: 2..kMaxMultiplier. SetConfig() normalises (< 2 with mode Fixed
		// -> disabled, > kMaxMultiplier -> kMaxMultiplier). Kept as chosen in the
		// other modes.
		int multiplier = 2;
		// Target: the output fps to aim for, 30..1000; 0 = the display's refresh.
		int targetFps = 0;
		Priority priority = Priority::Smoothness;
		// "Pause at refresh rate". true (default): the output is capped at the
		// refresh rate, so generation stops once the game alone reaches it. false:
		// no refresh cap -- a fixed multiplier keeps generating N-1 frames per real
		// pair however fast the game is, and a Target fps may exceed the refresh;
		// the extra frames are presented between vblanks where the backend can show
		// them (Pacing.h's "BEYOND THE REFRESH RATE").
		bool pauseAtRefresh = true;
		// "Limit to GPU speed": the cost guard (lower the output rate, or pass
		// real frames through, when generation takes too much of the GPU).
		bool gpuLimit = false;
		Quality quality = Quality::Quality;
		Safety safety = Safety::Off;
		HudProtect hud = HudProtect::Strong;
		UiProt ui = UiProt::Crosshair;
		// The crosshair box (Crosshair / CrosshairV2), tenths of a percent of the
		// game's height; clamped to [kUiBoxMinTenths, kUiBoxMaxTenths] by SetConfig().
		int uiBoxTenths = kUiBoxDefaultTenths;
	};

	// Replace the whole config. Thread-safe, cheap, callable at any time
	// (startup load, a user change, a profile switch). Takes effect at the next
	// vulkan_composite(): a preset change is applied live; a Quality change
	// additionally costs one waitIdle + reconfigure there. Turning it
	// off releases every resource at the next composite (and asks
	// for one, so it does not wait for the next vblank that happens to draw).
	void   SetConfig( const Config &cfg );
	Config GetConfig();

	// The per-frame gate: Config::enabled. One relaxed atomic load. The three
	// backends OR this into their "needs full composite" decision (direct
	// scanout must be impossible while frame generation is on: a scanned-out
	// buffer never passes through vulkan_composite()), and steamcompmgr must
	// not pre-emptively upscale layer 0 while it is true (a pre-upscaled
	// texture reaches vulkan_composite() already baked with
	// bBaseLayerEffectsApplied, which is past the point FG can substitute).
	bool Enabled();   // == Config::enabled (frame generation only)

	// Frame generation OR motion blur OR the lag spike buffer is on: the renderer is wanted, the backends
	// must full-composite, steamcompmgr must not pre-upscale, and the pacer is
	// driven. One relaxed atomic load.
	bool Active();

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
		// THE ONE RENDERER RULE (the pacer's Decision, applied as it says):
		//   prevId != 0 : recordSynthBlur( prev, curr, out, t0, t1 ) -- a plain synth
		//                 when t0 == t1 (blur off), and the real frame currId (blurred)
		//                 when t1 == 1. With blur on a REAL frame's output carries
		//                 prevId = the previous real frame, currId = that frame, t = 1.
		//   prevId == 0 : no renderer work; show the real frame showId.
		float t0 = 1.0f;
		float t1 = 1.0f;
		// The real frame to show when prevId == 0: the newest, unless a lag-spike
		// buffer delays (then an older frame still in the ring: it is substituted).
		uint64_t showId = 0;
		// Real frames the renderer's ring must hold (framegen::pacing::Pacer::
		// HistoryDepth(): 3, up to kHistoryMax = 65 with a buffer). The renderer
		// grows its ring (and the library's UI-protection history) at once when it
		// rises, and shrinks them only after it has stayed lower for a while, at a
		// point where it waits for the GPU anyway.
		int historyDepth = 3;
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
		Hdr,          // the game layer is a pass-through surface (never the case for a game's own layer 0 in practice)
		YCbCr,        // the game layer is a YCbCr (NV12) video surface (not offered, see frame-generation.md)
		Format,       // the game layer's pixel format is none of 8-bit RGB, 10-bit RGB, fp16 RGB (565, 16-bit UNORM)
		HdrFormat,    // 10-bit or fp16 game, but this GPU / driver cannot do that format for frame generation
		TooSmall,     // below Interpolator::kMinSize (16) on an axis
		InitFailed,   // the library (or its textures) could not be created / resized
		RecordFailed, // a record call returned false for this frame
	};

	// ------------------------------------------------------------------
	//  The UI-protection box preview (the Inspector's picture)
	// ------------------------------------------------------------------
	//
	// "What is under the box": a crop of the game's centre -- the box plus
	// context -- as the user would see it, so the box size can be judged against
	// the actual crosshair (the user: "show an image of the area that's underneath
	// the current UI, so he can see if the whole crosshair fits in"). The renderer
	// records the crop (cs_fg_crop.comp) into the command buffer it already
	// submits for a new real frame, at most ~10 times a second, and ONLY while the
	// overlay keeps asking (BoxPreviewWanted() each frame the picture is on
	// screen; it lapses by itself 400 ms after the last call). The pixels are read
	// back on a later composite once that command buffer has retired -- never a
	// wait. It needs the renderer to be running (Frame generation, Motion blur or
	// the Lag spike buffer on); otherwise there are simply no frames.
	constexpr uint32_t kBoxPreviewMax = 128;   // == rendervulkan.hpp's kUiCropMax

	struct BoxPreview
	{
		uint64_t ulGeneration = 0;      // 0 = never captured; bumped per capture
		uint64_t ulCapturedNs = 0;      // get_time_in_nanos() when the capture was published
		uint32_t uGameW = 0, uGameH = 0;   // the game's frame the crop is from
		uint32_t uBoxW = 0, uBoxH = 0;     // the box, game pixels
		uint32_t uW = 0, uH = 0;           // the picture, pixels (<= kBoxPreviewMax)
		uint32_t uFactor = 1;              // game pixels per picture pixel (1 = exact)
		// The box inside the picture, in picture pixels (fractional when uFactor > 1).
		float flBoxX = 0.0f, flBoxY = 0.0f, flBoxW = 0.0f, flBoxH = 0.0f;
		// The protected "work rectangle" (the box grown by the mode's margin: 2 px for
		// V1, 1 px for V2, none otherwise; clamped to the frame): game pixels, and its
		// place in the picture in picture pixels.
		uint32_t uWorkW = 0, uWorkH = 0;
		float flWorkX = 0.0f, flWorkY = 0.0f, flWorkW = 0.0f, flWorkH = 0.0f;
		bool bUiOn = true;                 // UI protection was on for this capture
		// uW x uH, tightly packed, RGBA, 8-bit sRGB, top row first.
		uint8_t rgba[ kBoxPreviewMax * kBoxPreviewMax * 4 ] = {};
	};

	// The overlay calls this every frame the picture is on screen. Cheap, any thread.
	void BoxPreviewWanted();
	// Copies the newest capture out when it is newer than ulHaveGeneration.
	bool GetBoxPreview( BoxPreview *pOut, uint64_t ulHaveGeneration );
	// The game's frame size as the renderer last saw it (false = it has not seen one).
	bool GameFrameSize( uint32_t *puWidth, uint32_t *puHeight );
	// The box's side in game pixels for a frame of w x h and a size in tenths of a
	// percent of the height: the library's own boxSize(), so the number is the one
	// it protects.
	void UiBoxPixels( uint32_t uWidth, uint32_t uHeight, int nTenths, uint32_t *puBoxW, uint32_t *puBoxH );

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
		// ONE synth (once per generated frame; a blurred output counts as its sample
		// count, so this is per SAMPLE then), milliseconds, < 0 = n/a. Cost of a
		// pair with g generated frames is estimate + g x synth; the pacing cost
		// guard budgets from these. lastSynthMs is the mean over the synths the
		// profile covered (the library's query pool holds about a dozen).
		float lastEstimateMs = -1.0f;
		float lastSynthMs = -1.0f;
		// The UI-protection share of the pair cost above (already INCLUDED in
		// lastPairGpuMs / lastEstimateMs / lastSynthMs, so the cost guard counts it):
		// one observe (ui_detect + ui_inpaint) plus one patch (ui_patch), ms.
		// < 0 = n/a (protection off, or not measured yet).
		float lastUiMs = -1.0f;
		// The game format the renderer is running in, for the Status line: "" for
		// plain 8-bit, else a short tag ("10-bit", "16-bit float", "HDR"). A string
		// literal; "" while the renderer is not live.
		const char *formatTag = "";
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
		WaitingForDecoder,   // video mode only (library PR #5): never occurs for gamescope's arrival mode
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
		// round(effectiveMultiplier), at least 2 while the pacer's plan is Generate, 0
		// while passing real frames through. It is the pacer's INSTANTANEOUS state: it
		// reads 2 at an effective 0.9x-1.27x too, so it is NOT "frame generation is
		// visibly adding frames" -- `generating` below is (the HUD's separator uses it).
		// (Since PR #16 `generating` is the Generate state plus a recent generated
		// frame, with no multiplier gate; this field stays the instantaneous state.)
		int   activeN = 0;
		float delayMs = 0.0f;     // D: the extra latency pacing adds, in milliseconds
		// Each feature separately (pacing's Report), for its own settings line.
		bool  fgActive = false;     // frame generation is on and generating
		bool  blurActive = false;   // motion blur is applied
		float blurWindowMs = 0.0f;  // the last shutter window, ms of content time
		int   blurSamples = 0;      // samples per output while blurActive
		float bufferDelayMs = 0.0f; // the lag-spike buffer's part of delayMs (0 until it exists)
		float bufferTargetMs = 0.0f; // what the buffer is ramping toward (0 with the buffer off)
		float lastSpikeMs = 0.0f;    // the newest spike inside the look-back window (0 = none)
		int   spikesInWindow = 0;    // spikes inside the window that size the buffer
		int   outliersIgnored = 0;   // spikes in the window above Max buffer: ignored for sizing
		int   historyFrames = 3;     // real frames the host keeps (pacer's HistoryDepth())
		PassReason reason = PassReason::Normal; // why it is not generating as asked, or pass-through
		// The library's steady "generating" signal (pacing.h Report::generating /
		// steadyMultiplier): the pacer is in the Generate state AND a generated frame
		// was presented within the last ~1 s. No multiplier gate (PR #16: the old
		// 1.2 / 1.1 outputs-per-real-frame constants are gone, "if it's generating
		// any frames, it should show the '>'"). Never true while passing through.
		// steadyMultiplier is informational only.
		float steadyMultiplier = 0.0f;
		bool  generating = false;
		// get_time_in_nanos() when this status was published (PublishPacingStatus stamps
		// it): the status is only republished about every 250 ms while the pacer is
		// being driven, so a reader treats an old one as "not generating".
		uint64_t publishedNs = 0;
	};

	// Mutex-protected snapshot, one writer (pacing), any reader (the panel).
	void         PublishPacingStatus( const PacingStatus &status );
	PacingStatus GetPacingStatus();

	// ------------------------------------------------------------------
	//  Renderer entry points -- vulkan_composite() ONLY
	// ------------------------------------------------------------------

	// True when vulkan_composite() should call RecordBaseLayer(): frame
	// generation or motion blur is on, OR it was on and still holds resources
	// that the next call must release. This is the single atomic load the off path pays.
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
	//   eColorspace : that layer's colourspace. With the layer's VkFormat it picks the
	//                 library format (FrameGenFormat.h): 8-bit, 10-bit and fp16 (HDR or
	//                 not) all run; YCbCr and pass-through surfaces are refused.
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
	//     hands it to the library's recordObserve() (UI protection);
	//   * for a FrameRequest with prevId != 0: the first output of a real pair
	//     records recordEstimate first, then recordSynthBlur(t0, t1) (a plain
	//     synth when t0 == t1) into a pooled output (decision D13) cached by
	//     outId, so a repeat composite of the same output records nothing; with
	//     prevId == 0 it shows the real frame showId (layer 0, or an older ring
	//     frame) and records nothing.
	gamescope::Rc<CVulkanTexture> RecordBaseLayer( gamescope::Rc<CVulkanTexture> pLayer0, GamescopeAppTextureColorspace eColorspace );
}
