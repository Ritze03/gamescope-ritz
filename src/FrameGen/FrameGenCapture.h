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
#include <deque>
#include <functional>
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
		int seconds = 0;           // the arm delay, 0 = start now; for `watch`: its length
		bool watch = false;        // `ritz_fg_capture watch [seconds] [sensitivity]`
		float sensitivity = 1.0f;  // watch only: divides the pixel trigger's threshold; 0 = every measured frame triggers
		std::string error;         // when !ok: one line for the caller
	};
	// `args` is the console's: [0] is the command name. No argument = start now;
	// one integer 0..kMaxArmSeconds = start that many seconds later;
	// `watch [seconds 1..600, default 60] [sensitivity >= 0, default 1]` = the watch mode.
	ArgResult ParseArgs( std::span<std::string_view> args );

	// ------------------------------------------------------------------
	//  Watch mode (`ritz_fg_capture watch`): telemetry, flags, the pixel
	//  trigger, the rolling image window. All of it pure; the session uses it.
	// ------------------------------------------------------------------

	constexpr int kWatchDefaultSeconds = 60;
	constexpr int kWatchMaxSeconds = 600;
	constexpr int kWatchMaxDumps = 3;               // dumps written per watch, at most
	constexpr int kWatchAfterTrigger = 12;          // records kept after the triggering one before the window freezes
	constexpr int kWatchRecordsPerSecond = 500;     // telemetry reserved per second of watch (more are counted as dropped)
	constexpr uint32_t kMetricBlock = 16;           // the pixel trigger's block size (scan.py: 16x16)
	constexpr uint32_t kMetricStep = 4;             // every 4th pixel in x and y: 16 samples per block
	constexpr float kMetricBase = 6.0f;             // threshold = ( kMetricBase + kMetricPairGain * pair ) / sensitivity
	constexpr float kMetricPairGain = 1.5f;

	// Host-state anomaly flags of one telemetry record.
	enum WatchFlag : uint32_t
	{
		kFlagGap = 1u << 0,           // currId != prevId + 1
		kFlagBack = 1u << 1,          // currId or outId smaller than in the previous record
		kFlagStale = 1u << 2,         // a reused estimate whose ring frames were rewritten since it was recorded
		kFlagRefused = 1u << 3,       // recordEstimate or the synth returned false
		kFlagNear = 1u << 4,          // an over-the-cap repaint of the nearest output
		kFlagPairMismatch = 1u << 5,  // the slots read / the output repainted do not hold the pair asked for
		kFlagRepeat = 1u << 6,        // a newly stored real frame is identical to an earlier one with a different frame between (set by the analysis)
		kFlagStatic = 1u << 7,        // a newly stored real frame is identical to the one(s) right before it (a still picture)
	};
	constexpr int kFlagKinds = 8;
	constexpr uint32_t kDumpFlags = kFlagStale | kFlagRefused | kFlagPairMismatch | kFlagRepeat;   // these trigger a dump
	const char *FlagName( int nBit );                  // "gap", "back", ... ("?" out of range)
	std::string FormatFlags( uint32_t uFlags );         // "gap,near" or "-"

	struct FlagInput
	{
		uint64_t ulOutId = 0, ulPrevId = 0, ulCurrId = 0;
		bool bEstimateRecorded = false;   // false = reused
		int nEstimateRet = -1, nSynthRet = -1;
		bool bNear = false;
		bool bStaleHost = false;          // the host saw a reused estimate over rewritten ring slots
		bool bMismatchHost = false;       // the host saw slots / an output holding another pair
	};
	// The flags of one record; `*pulLastCurr` / `*pulLastOut` carry the previous record's ids (0 = none) and are updated.
	uint32_t EvalFlags( const FlagInput &in, uint64_t *pulLastCurr, uint64_t *pulLastOut );

	// Block means: one float per colour channel (3) per 16x16 block, estimated from every kMetricStep-th
	// pixel; the channels on a 0..255 scale whatever the format (10-bit / 4, fp16 * 255). Only whole blocks
	// (W / 16 x H / 16), as scan.py does.
	struct BlockMeans
	{
		uint32_t uBw = 0, uBh = 0;
		std::vector<float> v;   // ( y * uBw + x ) * 3 + c
	};
	// False for a format the capture cannot read or an image smaller than one block.
	bool ComputeBlockMeans( const uint8_t *pPixels, uint32_t uPitch, uint32_t uWidth, uint32_t uHeight, int nVkFormat, uint32_t uStep,
		BlockMeans *pOut );
	// Mean absolute difference of two block-mean sets (`pair`); -1 when they do not match in size.
	float BlockDiff( const BlockMeans &a, const BlockMeans &b );
	// Mean absolute difference of `out` against the blend ( 1 - t ) * prev + t * curr (`coarse`); -1 on a size mismatch.
	float BlendDiff( const BlockMeans &out, const BlockMeans &prev, const BlockMeans &curr, float t );
	// ( kMetricBase + kMetricPairGain * pair ) / sensitivity; 0 for sensitivity <= 0.
	float Threshold( float flPair, float flSensitivity );
	// coarse > threshold; ALWAYS true for sensitivity <= 0 (the live test of the trigger path).
	bool PixelTriggers( float flCoarse, float flPair, float flSensitivity );

	// The `repeat` test: a real frame whose content equals an earlier real frame (same sparse block means AND
	// the same hash of three full rows) is a repeat when a frame BETWEEN the two differs (A B A), and `static`
	// when every frame between is the same too (A A: a still picture). Frames are fed in arrival order.
	uint64_t RowHash( const uint8_t *pPixels, uint32_t uPitch, uint32_t uWidth, uint32_t uHeight, uint32_t uBytesPerPixel );
	class RepeatDetector
	{
	public:
		enum Kind { kNone, kRepeat, kStatic };
		struct Result { Kind eKind = kNone; uint64_t ulSameAs = 0; };
		static constexpr size_t kHistory = 16;   // real frames compared against
		// A real frame seen for the first time; the same id again is ignored (None).
		Result Add( uint64_t ulId, const BlockMeans &m, uint64_t ulRowHash );
	private:
		struct Sig { uint64_t ulId; BlockMeans m; uint64_t ulHash; };
		std::deque<Sig> m_Hist;   // oldest first
	};

	// One telemetry record (one composite that showed, or tried to show, a generated frame).
	struct WatchRec
	{
		uint64_t ulIndex = 0, ulNs = 0;
		uint64_t ulOutId = 0, ulPrevId = 0, ulCurrId = 0, ulNewestId = 0;
		float flT0 = 1.0f, flT1 = 1.0f;
		int64_t nArrivalPrevNs = -1, nArrivalCurrNs = -1;
		bool bEstimateRecorded = false;
		int8_t nEstimateRet = -1, nSynthRet = -1;
		bool bCached = false, bNear = false;
		int8_t nOutSlot = -1, nPrevSlot = -1, nCurrSlot = -1;
		uint32_t uFlags = 0;
		float flCoarse = -1.0f, flPair = -1.0f;   // < 0: not measured (`na`)
	};
	std::string FormatWatchLine( const WatchRec &r );

	struct WatchSummary
	{
		uint64_t ulRecords = 0, ulMeasured = 0, ulNotMeasured = 0, ulDroppedRecords = 0;
		uint64_t ulNotMeasuredDumping = 0;   // of ulNotMeasured: while a dump was being written
		int nDumps = 0;
		uint64_t ulTriggers = 0;             // dump triggers seen (also those that could not dump)
		float flMaxRatio = -1.0f;            // the largest coarse / threshold (threshold at sensitivity 1), -1 = none measured
		WatchRec maxRec;                     // the record with that ratio
		uint64_t ulFlagCount[ kFlagKinds ] = {};
	};
	// Folds one finished record into the summary (measured or not, its flags).
	void SummaryAdd( WatchSummary *pS, const WatchRec &r );

	// The pair's arrival span (arrival of curr - arrival of prev, ns); -1 when either arrival is unknown.
	int64_t PairSpanNs( const WatchRec &r );
	// The spans of a watch: the median over the records with a known span, the largest (with its record), and how
	// many DISTINCT pairs (prevId, currId) had a span above 2, 3, 5 and 8 times that median.
	struct SpanStats
	{
		uint64_t ulKnown = 0;            // records with a known span
		double flMedianMs = -1.0;        // -1 = none known
		double flMaxMs = -1.0;
		uint64_t ulMaxRec = 0, ulMaxPrevId = 0, ulMaxCurrId = 0;
		uint64_t ulPairsOver[ 4 ] = {};  // x2, x3, x5, x8
	};
	SpanStats ComputeSpanStats( const std::vector<WatchRec> &recs );
	// `span_known= span_median_ms= span_max_ms= span_max_record= span_max_prevId= span_max_currId= span_gt2= span_gt3= span_gt5= span_gt8=`
	std::string FormatSpanTokens( const SpanStats &s );
	// The summary line; `sExtra` (the span tokens) goes before `reason=`, which stays last.
	std::string FormatWatchSummary( const WatchSummary &s, const char *pszReason, const std::string &sExtra = std::string() );
	// `trigger kind= record= seq= coarse= pair= threshold=` (a dump's capture.txt). seq = -1: the triggering
	// composite has no frame in the dump (a refused synth has no image).
	// A `repeat` trigger also names the two identical frames: `... id=<new> same_as=<old>` (when ulId != 0).
	std::string FormatTriggerLine( const char *pszKind, uint64_t ulRecord, int nSeq, float flCoarse, float flPair, float flThreshold,
		uint64_t ulId = 0, uint64_t ulSameAs = 0 );
	// The free space a watch needs: kWatchMaxDumps bursts' images plus the telemetry text.
	uint64_t WatchRequiredBytes( uint32_t uWidth, uint32_t uHeight, uint32_t uBytesPerPixel, int nSeconds );

	// The rolling image window: nOuts output staging images (one per record) and nReals real-frame staging
	// images (shared by the records that use the same commit id). Add() puts a record in, evicting the oldest
	// records while the window is full or no real slot is free; a real slot is reused only when no record in
	// the window refers to it. `canEvict( recordIndex )` may forbid an eviction (the record was not analysed
	// yet, or it triggers a dump): Add() then fails and changes nothing but the evictions already allowed.
	class RollingWindow
	{
	public:
		struct Entry
		{
			uint64_t ulRec = 0;
			int nOut = -1;
			uint64_t ulId[ 2 ] = {};
			int nReal[ 2 ] = { -1, -1 };
		};
		struct AddResult
		{
			bool ok = false;
			int nOut = -1;
			int nReal[ 2 ] = { -1, -1 };
			bool bRealNew[ 2 ] = {};   // the real must be copied into nReal[i] now
			bool bBlocked = false;     // failed because canEvict() refused the oldest record
		};
		RollingWindow( int nOuts, int nReals );
		AddResult Add( uint64_t ulRec, uint64_t ulPrevId, uint64_t ulCurrId, const std::function<bool( uint64_t )> &canEvict );
		const std::vector<Entry> &Entries() const { return m_Entries; }   // oldest first
		void Clear();
		int RefCount( int nReal ) const { return m_nRef[ size_t( nReal ) ]; }
		uint64_t RealId( int nReal ) const { return m_ulRealId[ size_t( nReal ) ]; }
	private:
		void EvictFront();
		int m_nOuts;
		std::vector<int> m_nRef;
		std::vector<uint64_t> m_ulRealId;   // the commit id a real slot holds (0 = none)
		std::vector<bool> m_bOutUsed;
		std::vector<Entry> m_Entries;
		int m_nRealCursor = 0;
	};

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
		int nBlend = 0;              // V3 Hybrid's variant (1 = Fluidity, 2 = Consistency), 0 = none: no `blend=` token
		bool bBlendOff = false;      // the variant is selected but the library runs V3's rule: the token reads `blend=Fluidity(off)`
		int nV35 = 0;                // V3.5 requested (1), 0 = no `v3motion=` token
		bool bV35Off = false;        // requested but not running: the token reads `v3motion=V3.5(off)`
		int nView = 0;               // the Debug view the library draws (the row's index: 1 UI highlighted, 2 UI removed, 3 Motion map), 0 = none: no `view=` token
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
	// `watch v=1 ...`: the capture header's identity tokens plus seconds, sensitivity and the threshold formula.
	std::string FormatWatchHeader( const HeaderInfo &h, int nSeconds, float flSensitivity );
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
		bool bNear = false;               // an over-the-cap repaint of the nearest output (`near=1`)
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
	// Console thread. Starts a watch of `nSeconds` (prepared on the next live composite); false = refused.
	bool RequestWatch( int nSeconds, float flSensitivity, std::string *psMessage );

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
		int nBlend = 0;                            // V3 Hybrid's variant selected (0 = none), see HeaderInfo::nBlend
		bool bBlendActive = true;                  // Interpolator::v3BlendActive() at the start
		int nV35 = 0;                              // V3.5 requested (HeaderInfo::nV35)
		bool bV35Active = true;                    // Interpolator::v3MotionActive() at the start
		int nView = 0;                             // the Debug view in effect (HeaderInfo::nView)
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
		bool bNear = false;          // the over-the-cap repaint of the nearest output (ids / t: the output's own)
		bool bStaleHost = false;     // watch flags, see FlagInput
		bool bMismatchHost = false;
		int nOutSlot = -1, nPrevSlot = -1, nCurrSlot = -1;
	};
	// Records the staging copies of one generated frame into `pCmd` (the composite's own command buffer,
	// AFTER the synth or the cached output's repaint), or counts the frame as dropped. The record is
	// held until Submitted(). `pOut` null (a refused estimate / synth): telemetry only (watch), no images.
	void RecordFrame( CVulkanCmdBuffer *pCmd, const FrameIn &in );
	// The command buffer that carries the copies was submitted as `ulSeq`: hand the records to the writer.
	void Submitted( uint64_t ulSeq );
	// Ends a running capture now (teardown, resize, format change); what was recorded is still written.
	void Abort( const char *pszReason );
}
