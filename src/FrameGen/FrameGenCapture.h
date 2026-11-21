#pragma once

// FrameGenCapture.h -- the `ritz_fg_capture` troubleshooting aid
// (superdoc/features/frame-generation-capture.md): record a burst of generated
// frames with the real frames they were made from, the library settings and the
// pacer's recent decisions, to files, so the exact inputs of a defect can be
// replayed through the library offline.
//
// This header is the whole surface. The part WITHOUT Vulkan (the always-on
// decision ring and arrival table, the argument parsing, the free-space
// arithmetic, every text format and the tight-row writer) lives in
// FrameGenCapture.cpp and is unit tested (tests/test_framegen_capture.cpp). The
// part that touches the GPU (staging copies recorded into the composite's own
// command buffer, the writer thread) lives in FrameGenCaptureSession.cpp.
//
// THREADS. Request() runs on the console thread and only sets atomics. The
// decision ring and the arrival table are written by the steamcompmgr thread
// (pacing); the session (Poll .. Abort) belongs to the thread that runs
// vulkan_composite(); the writer thread is the session's own.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rc.h"

class CVulkanTexture;
class CVulkanCmdBuffer;

namespace framegen { struct Settings; }

namespace fghost::capture
{
	// ------------------------------------------------------------------
	//  Constants
	// ------------------------------------------------------------------

	constexpr int kFrames = 40;                      // N: generated frames in one burst
	constexpr int kDecisionRing = 256;               // pacer decisions kept, always on
	constexpr int kDecisionsDumped = 200;            // the last of them written before the start
	constexpr int kArrivalSlots = 16;                // commit id -> arrival time, the last ones
	constexpr int kMaxArmSeconds = 600;              // `ritz_fg_capture <seconds>`: 0..600
	constexpr uint64_t kTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;   // no generated frame this long: the burst ends
	constexpr uint64_t kReserveBytes = 64ull * 1024ull * 1024ull;          // free space needed on top of the images
	constexpr int kVersion = 1;                      // capture.txt's `v=`

	// ------------------------------------------------------------------
	//  Always-on: the pacer's decisions and the real frames' arrivals
	// ------------------------------------------------------------------

	// One pacer Decision, as a small POD (no allocation to store it).
	struct DecisionRec
	{
		uint64_t counter = 0;       // 1, 2, 3 ... in the order pushed (set by DecisionRing::Push)
		uint64_t ns = 0;            // monotonic, when pacing decided
		uint64_t newestId = 0, prevId = 0, currId = 0, outId = 0, showId = 0;
		float t = 1.0f, t0 = 1.0f, t1 = 1.0f;
		bool inert = false, skip = false, reset = false, repaintNext = false, newOutput = false;
		// No decision was made at all: layer 0 was not a plain game frame (fade, UI,
		// streaming client, no commit yet) and the renderer was told to stay inert.
		bool discontinuity = false;
		// The reason of the pacer's LAST pacing status (published about every 250 ms), not
		// of this decision: the Decision struct carries none. A framegen::pacing::Reason value.
		uint8_t reason = 0;
	};

	// Generate or pass, from the decision's own fields: a pair is named, the output is
	// not a real frame (t < 1) and the renderer is neither inert nor skipped.
	bool DecisionGenerates( const DecisionRec &r );
	// The enumerator name of a framegen::pacing::Reason value ("Normal", "GameTooFast", ...).
	const char *ReasonName( uint8_t uReason );

	// A fixed ring: Push() overwrites the oldest, never allocates. One tiny mutex held for a
	// POD copy; nothing else ever takes it for longer (the console thread never does).
	class DecisionRing
	{
	public:
		void Push( DecisionRec r );
		// The newest `nMax` records, OLDEST FIRST.
		std::vector<DecisionRec> Snapshot( size_t nMax ) const;
		uint64_t Count() const;   // pushed so far (not capped by the ring size)
	private:
		mutable std::mutex m_Mutex;
		DecisionRec m_Buf[ kDecisionRing ];
		uint64_t m_ulCount = 0;
	};

	// Commit id -> arrival time (ns, monotonic) of the last kArrivalSlots real frames: the
	// library's pacer keeps its own, but exposes none, so the host keeps a copy.
	class ArrivalTable
	{
	public:
		void Note( uint64_t ulId, uint64_t ulNs );
		int64_t Find( uint64_t ulId ) const;   // -1 when the id is not (or no longer) in the table
	private:
		mutable std::mutex m_Mutex;
		uint64_t m_ulId[ kArrivalSlots ] = {};
		uint64_t m_ulNs[ kArrivalSlots ] = {};
		uint32_t m_uNext = 0;
	};

	DecisionRing &Decisions();
	ArrivalTable &Arrivals();

	// ------------------------------------------------------------------
	//  Pure helpers
	// ------------------------------------------------------------------

	struct ArgResult
	{
		bool ok = false;
		int seconds = 0;           // the arm delay, 0 = start now
		std::string error;         // when !ok: one line for the caller
	};
	// `args` is the console's: [0] is the command name. No argument = start now;
	// one integer 0..kMaxArmSeconds = start that many seconds later.
	ArgResult ParseArgs( std::span<std::string_view> args );

	// (N reals-or-outs images) x w x h x bpp, plus kReserveBytes: N outputs, N + 2 real frames.
	uint64_t RequiredBytes( uint32_t uWidth, uint32_t uHeight, uint32_t uBytesPerPixel, int nFrames );
	bool HasEnoughSpace( uint64_t ulAvailableBytes, uint64_t ulRequiredBytes );

	// The VkFormat numbers the renderer can run in (8-bit, 10-bit, fp16): name and bytes per pixel.
	// An unknown number gives "UNKNOWN" and 0.
	const char *VkFormatName( int nVkFormat );
	uint32_t VkFormatBytes( int nVkFormat );

	std::string OutFileName( int nSeq );          // "000_out.bgra"
	std::string RealFileName( uint64_t ulId );    // "id123_real.bgra"

	struct HeaderInfo
	{
		std::string sWallStart;      // "2026-11-21T12:34:56+0100"
		uint64_t ulStartNs = 0;      // monotonic
		uint32_t uWidth = 0, uHeight = 0;
		int nVkFormat = 0;
		uint32_t uBytesPerPixel = 0;
		std::string sLibraryCommit;  // "unknown" when the build could not tell
		std::string sRitzVersion;    // the About area's version (the changelog's top heading)
		std::string sRitzCommit;
		bool bRitzDirty = false;
		std::string sMotionWanted;   // what the user chose
		std::string sMotionEffective;   // what the library runs (V1 when it refused the wish)
		uint32_t uFlowScale = 0;
		int nFrames = kFrames;
		float flPrepareMs = 0.0f;      // how long the space check + staging allocation + directory took
		bool bPreparedEarly = false;   // done when armed (before the start time), not at the start
	};
	// The largest gap between two consecutive decisions' times (ns), and the counter of the decision after the gap
	// (0 when fewer than two records).
	uint64_t MaxDecisionGapNs( const std::vector<DecisionRec> &v, uint64_t *pulAfterCounter );
	// `timing first_frame_ns= last_frame_ns= burst_ns= max_decision_gap_ns= gap_after_n= last_decision_to_start_ns=`
	std::string FormatTimingLine( uint64_t ulFirstNs, uint64_t ulLastNs, uint64_t ulMaxGapNs, uint64_t ulGapAfterCounter, uint64_t ulLastDecisionToStartNs );
	std::string FormatHeaderLine( const HeaderInfo &h );
	// One `setting name=value` line for EVERY field of framegen::Settings (enums also `text=Name`).
	std::vector<std::string> FormatSettingLines( const framegen::Settings &s );
	std::string FormatDecisionLine( const DecisionRec &r );

	struct FrameInfo
	{
		int nSeq = 0;
		uint64_t ulOutId = 0, ulPrevId = 0, ulCurrId = 0, ulNewestId = 0;
		float flT0 = 1.0f, flT1 = 1.0f;
		int64_t nArrivalPrevNs = -1, nArrivalCurrNs = -1;   // -1 = unknown
		uint64_t ulNowNs = 0;
		uint32_t uWidth = 0, uHeight = 0;
		bool bEstimateRecorded = false;   // false = "reused" (an earlier composite's estimate)
		int nEstimateRet = -1;            // recordEstimate's return when recorded, else -1 (printed "na")
		int nSynthRet = -1;               // recordSynthBlur's return
		bool bCached = false;             // the output was a cached slot (a repaint), no synth this composite
		int nOutSlot = -1, nPrevSlot = -1, nCurrSlot = -1;
		std::string sOutFile, sPrevFile, sCurrFile;
	};
	std::string FormatFrameLine( const FrameInfo &f );
	std::string FormatEndLine( int nFrames, int nReals, int nDropped, const char *pszReason );

	// Writes `uHeight` rows of `uRowBytes` bytes each from `pPixels` (rows `uPitch` bytes apart)
	// to `f`, tightly packed. False on a short write or an error.
	bool WriteTightRows( FILE *f, const uint8_t *pPixels, uint32_t uPitch, uint32_t uRowBytes, uint32_t uHeight );

	// ------------------------------------------------------------------
	//  The session (FrameGenCaptureSession.cpp)
	// ------------------------------------------------------------------

	// Console thread. Arms (or starts) a capture; false = refused, with the reason in *psMessage.
	bool Request( int nSeconds, std::string *psMessage );

	// A capture is armed, running or still writing. RenderWanted() includes it, so the
	// renderer entry point runs (and can time out / clean up) even with frame generation off.
	bool Pending();

	// Everything the header needs that only the renderer knows; filled when a capture starts.
	struct Env
	{
		uint32_t uWidth = 0, uHeight = 0;
		int nVkFormat = 0;
		uint32_t uDrmFormat = 0;                   // the ring's format: the staging images are created with it
		const framegen::Settings *pSettings = nullptr;   // the library's settings in effect
		const char *pszMotionWanted = "";
		const char *pszMotionEffective = "";
	};

	// Paint thread, once per RecordBaseLayer() call, first. Times out an armed capture that could not
	// start, a burst that waits for frames in vain, a changed configuration (`ulConfigKey`: any value
	// that differs between two settings), and frees a finished one.
	void Poll( uint64_t ulNowNs, uint64_t ulConfigKey );
	// True when an armed capture's time has come and it should be started by Start().
	bool DueToStart( uint64_t ulNowNs );
	// Starts the burst (space check, staging allocation, directory, writer thread). False: refused (logged),
	// nothing written, back to idle.
	bool Start( const Env &env, uint64_t ulNowNs, uint64_t ulConfigKey );
	bool Running();

	struct FrameIn
	{
		gamescope::Rc<CVulkanTexture> pOut;    // the output slot's texture, as the library returned it
		gamescope::Rc<CVulkanTexture> pPrev;   // the ring copies of the pair (null when the ring no longer holds it)
		gamescope::Rc<CVulkanTexture> pCurr;
		uint64_t ulOutId = 0, ulPrevId = 0, ulCurrId = 0, ulNewestId = 0;
		float flT0 = 1.0f, flT1 = 1.0f;
		int nEstimateRet = -1;   // recordEstimate's return when it was recorded in this composite, else -1
		int nSynthRet = -1;
		bool bCached = false;
		int nOutSlot = -1, nPrevSlot = -1, nCurrSlot = -1;
	};
	// Records the staging copies of one generated frame into `pCmd` (the composite's own command buffer,
	// AFTER the synth or the cached output's repaint), or counts the frame as dropped. The record is
	// held until Submitted().
	void RecordFrame( CVulkanCmdBuffer *pCmd, const FrameIn &in );
	// The command buffer that carries the copies was submitted as `ulSeq`: hand the records to the writer.
	void Submitted( uint64_t ulSeq );
	// Ends a running capture now (teardown, resize, format change); what was recorded is still written.
	void Abort( const char *pszReason );
}
