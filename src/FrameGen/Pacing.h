#pragma once

// Pacing.h -- the pure decision logic of frame-generation pacing.
//
// Header-only, no gamescope globals, no Vulkan, no clock of its own: every
// time is passed in (nanoseconds, any monotonic base), so tests/test_framegen_
// pacing.cpp drives it with a fake clock. steamcompmgr.cpp is the thin glue
// that feeds it commit arrivals and paints and takes back what to show.
//
// THE MODEL: PER-VBLANK FRACTIONAL PACING (rewritten 2026-10-04).
// There is no "N" inside the pacer. Every output frame -- every vblank that
// the output cadence says should get a new image -- is generated for its OWN
// moment between two real frames:
//
//   * a real frame carries its arrival time (the commit's acquire fence
//     signalling, see OnArrival); the frames in play are A0 (before) and A1;
//   * an output frame painted for the vblank at time V shows CONTENT TIME
//         tau = V - D          (D = the delay, per priority, see below)
//     which lies between two real frames: t = (tau - A0) / (A1 - A0);
//   * t >= 1 (tau has caught up with the newest real frame): show that real
//     frame itself, no synth. Otherwise a synth at t.
//
// So "how many generated frames per real frame" is never chosen or held: it
// falls out of t from the latest arrivals, on the very next pair. A fixed
// multiplier N only sets the OUTPUT INTERVAL o = max(interval / N, 1/refresh)
// -- more frames than the display can show simply saturate at the refresh
// rate, nothing steps down (user direction #4). Target fps mode sets
// o = 1 / min(target, refresh). When o is longer than one vblank only the
// vblanks where a phase accumulator crosses o get a new image (Bresenham), so
// the average output rate is exact.
//
// DELAY D, per Priority (the user's "one for low latency and one for maximum
// smoothness"):
//   Low latency : D = latest real interval - o, floored at 0. At a fixed N this
//     is today's (N-1)/N of a game interval. A real frame arriving early or
//     late SNAPS the content time to the new pair rather than sliding (D9's
//     latency-first rule), so uneven motion on jittery frame times is accepted.
//   Smoothness  : D = median interval + margin (margin = max(2 ms, the spread
//     of recent intervals), capped at half an interval). tau advances evenly at
//     the output rate; the pair being played may be one OLDER than the newest
//     (so frames are queued by about one game interval). A late real frame
//     holds at t = 1 instead of hitching; an early one is absorbed by the
//     margin. Costs about one game frame of delay.
//   Why the pair can be older only in Smoothness: with D >= one interval the
//   newest real frame has usually ARRIVED while tau is still inside the pair
//   before it. The renderer therefore keeps the last three real frames.
//
// tau is MONOTONIC: an output frame never shows content older than the one
// before it (tau >= tau_last), however D moves.
//
// PASS-THROUGH (the real frame, shown at once, renderer inert) only when:
//   - the game's rate already reaches the output rate (interval <= o, with a
//     ratio band for hysteresis: stop generating below 1.02, start again above
//     1.10 -- NOT a time hold);
//   - the renderer is unavailable (HDR, 10-bit, ... -- it is still driven, so
//     it re-checks);
//   - the cost guard trips (below).
// VRR / tearing are NOT a pass-through reason: while generating, gamescope
// paces its own output frames on the vblank timer exactly as on a fixed-refresh
// display (PaintTick() below is the one rule the main loop uses), and while
// passing through it behaves as it always did.
//
// COST GUARD (D16, adapted). The renderer reports the estimate's and one
// synth's GPU time. Generation per game interval is estimate + the synths:
// when that would exceed 25% of the interval, the output rate is LOWERED
// (o raised) until it fits; if not even one generated frame per pair fits it
// passes through. A probe retries every 10 s. No timestamps, no guard.
//
// LATENCY. A newer real frame is seen by the very next paint: nothing is ever
// queued except by Smoothness's deliberate delay.
//
// Phase A is vblank-driven for every backend. "The vblank" enters through
// Inputs::vblankNs only, so a timer-paced Phase B can feed its own present
// time with no other change: tau = V - D is exactly as valid for it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <climits>

namespace fgpacing
{
	using Ns = uint64_t;

	constexpr Ns kNsPerMs = 1'000'000ull;

	// D14: the game's frame interval = median of the last kIntervalWindow arrival
	// intervals, clamped to [4, 50] ms (250 .. 20 fps).
	constexpr int    kIntervalWindow  = 8;
	constexpr int    kMinSamples      = 4;      // intervals needed before the estimate is trusted
	constexpr double kMinIntervalMs   = 4.0;
	constexpr double kMaxIntervalMs   = 50.0;
	// D12: a gap longer than this between real frames drops the previous frame.
	constexpr Ns     kGapResetNs      = 100 * kNsPerMs;
	// Generation stops when game interval / output interval falls below
	// kStopRatio (the game reaches the output rate) and starts again once it is
	// back above kStartRatio. 98% .. 91% of the refresh at a fixed multiplier.
	constexpr double kStopRatio       = 1.02;
	constexpr double kStartRatio      = 1.10;
	// D16: generation (estimate + synths) may use at most this share of a game
	// interval.
	constexpr double kCostBudget      = 0.25;
	// After a cost-guard pass-through nothing is generated, hence nothing is
	// measured; retry this often (one generated frame per pair) to see whether
	// the load has gone, and give up on a probe that measured nothing after
	// kCostProbeMaxNs.
	constexpr Ns     kCostProbeNs     = 10'000'000'000ull;
	constexpr Ns     kCostProbeMaxNs  = 3'000'000'000ull;
	// Status is worth publishing this often (D18).
	constexpr Ns     kStatusPeriodNs  = 250 * kNsPerMs;

	// The largest fixed multiplier (FrameGenHost.h kMaxMultiplier).
	constexpr int    kMaxN            = 8;
	// Smoothness: margin = max(kMinMarginMs, spread), at most half an interval.
	constexpr double kMinMarginMs     = 2.0;
	// Generated frames per real pair, well under the library's 32-set ring.
	constexpr int    kMaxSynthsPerPair = 24;
	// A synth is never made closer than this to either real frame; a newest-pair
	// t above kRealSnapT shows the real frame itself.
	constexpr double kTMin            = 0.02;
	constexpr double kRealSnapT       = 0.98;
	// Real frames kept for pair selection (the renderer's ring is the same size).
	constexpr int    kHistory         = 3;

	enum class Mode : uint8_t { Off, Fixed, Target };
	enum class Priority : uint8_t { LowLatency, Smoothness };

	// Mirrors fghost::PassReason value for value (steamcompmgr.cpp static_asserts
	// it); duplicated so this header stays free of the host's includes.
	enum class Reason : uint8_t
	{
		Normal,
		Off,
		WarmingUp,
		CostGuard,
		GameTooFast,
		GameStalled,
		RendererUnavailable,
	};

	// ------------------------------------------------------------------
	//  Pure helpers
	// ------------------------------------------------------------------

	inline double ClampIntervalMs( double flMs )
	{
		return std::min( std::max( flMs, kMinIntervalMs ), kMaxIntervalMs );
	}

	// The sorted copy's helpers. n is tiny (<= 8): insertion sort.
	inline int SortedCopy( const double *v, int n, double *s )
	{
		n = std::min( n, kIntervalWindow );
		for ( int i = 0; i < n; i++ )
		{
			int j = i;
			while ( j > 0 && s[ j - 1 ] > v[ i ] )
			{
				s[ j ] = s[ j - 1 ];
				j--;
			}
			s[ j ] = v[ i ];
		}
		return n;
	}

	// Median of v[0..n): the middle value, or the mean of the two middle ones
	// for an even count.
	inline double MedianOf( const double *v, int n )
	{
		if ( n <= 0 )
			return 0.0;
		double s[ kIntervalWindow ];
		n = SortedCopy( v, n, s );
		return ( n & 1 ) ? s[ n / 2 ] : 0.5 * ( s[ n / 2 - 1 ] + s[ n / 2 ] );
	}

	// A robust spread of v[0..n): second-largest minus median, so one hitch in
	// the window does not count but a jittery game does. 0 for fewer than 3.
	inline double SpreadOf( const double *v, int n )
	{
		if ( n < 3 )
			return 0.0;
		double s[ kIntervalWindow ];
		n = SortedCopy( v, n, s );
		return std::max( 0.0, s[ n - 2 ] - MedianOf( v, n ) );
	}

	// The output interval o in milliseconds, before the cost guard.
	//   Fixed : max(interval / n, 1 / refresh)  -- never faster than the display
	//   Target: 1 / min(target, refresh); target 0 = the display's refresh
	inline double OutputIntervalMs( Mode eMode, int nMultiplier, int nTargetFps, double flIntervalMs, double flRefreshHz )
	{
		const double flVblankMs = 1000.0 / std::max( flRefreshHz, 1.0 );
		if ( eMode == Mode::Target )
		{
			const double flTarget = nTargetFps > 0 ? std::min( double( nTargetFps ), flRefreshHz ) : flRefreshHz;
			return 1000.0 / std::max( flTarget, 1.0 );
		}
		const int n = std::max( nMultiplier, 1 );
		return std::max( flIntervalMs / double( n ), flVblankMs );
	}

	// Low latency: the latest real interval minus the output interval, floored.
	inline double LowLatencyDelayMs( double flLatestIntervalMs, double flOutputMs )
	{
		return std::max( 0.0, flLatestIntervalMs - flOutputMs );
	}

	// Smoothness: the median interval plus the margin.
	inline double SmoothnessMarginMs( double flIntervalMs, double flSpreadMs )
	{
		return std::min( std::max( kMinMarginMs, flSpreadMs ), 0.5 * flIntervalMs );
	}
	inline double SmoothnessDelayMs( double flIntervalMs, double flSpreadMs )
	{
		return flIntervalMs + SmoothnessMarginMs( flIntervalMs, flSpreadMs );
	}

	// How many generated frames per game interval the GPU budget allows:
	// floor((25% of the interval - estimate) / one synth); 0 = not even one;
	// INT_MAX = unknown (no guard). Costs < 0 = no timings.
	inline int MaxSynthsPerPair( double flIntervalMs, double flEstimateMs, double flSynthMs )
	{
		if ( flEstimateMs < 0.0 || flSynthMs < 0.0 )
			return INT_MAX;
		const double flRoom = kCostBudget * flIntervalMs - flEstimateMs;
		if ( flRoom <= 0.0 )
			return 0;
		const double flSynth = std::max( flSynthMs, 1e-4 );
		return int( std::min( std::floor( flRoom / flSynth + 1e-9 ), double( kMaxSynthsPerPair ) ) );
	}

	// Which main-loop iterations may paint. On a fixed-refresh display only the
	// vblank-timer ticks do (bLoopVblank == bFromTimer). Under VRR the loop treats
	// EVERY iteration as a vblank (a commit arrival paints at once) and a tearing
	// surface paints on arrival too; while frame generation is generating, its
	// output frames would then go out at commit-arrival times, bunched, with the
	// content times in the wrong places. So while generating (bGenerating) only
	// timer ticks paint -- the display is still asked for nothing (VRR stays on,
	// the flip is simply a normal one at the tick), and while passing through the
	// loop behaves exactly as without frame generation.
	inline bool PaintTick( bool bGenerating, bool bFromTimer, bool bLoopVblank )
	{
		return bGenerating ? bFromTimer : bLoopVblank;
	}

	// Ratio of the game's interval to the output interval: > 1 means there is
	// room to generate; the multiplier the display would be fed.
	inline double OutputRatio( double flIntervalMs, double flOutputMs )
	{
		return flOutputMs > 0.0 ? flIntervalMs / flOutputMs : 0.0;
	}

	// ------------------------------------------------------------------
	//  Interval estimator (D14)
	// ------------------------------------------------------------------

	// Fed one real-frame ARRIVAL time per frame. Not vblank latch times: those are
	// quantised to the refresh (+-7 ms for 60 fps on 144 Hz), arrival is not.
	class IntervalEstimator
	{
	public:
		void Clear()
		{
			m_nCount = 0;
			m_nHead = 0;
		}

		void AddInterval( Ns ulIntervalNs )
		{
			m_flMs[ m_nHead ] = double( ulIntervalNs ) / double( kNsPerMs );
			m_nHead = ( m_nHead + 1 ) % kIntervalWindow;
			m_nCount = std::min( m_nCount + 1, kIntervalWindow );
		}

		int  Count() const { return m_nCount; }
		bool Valid() const { return m_nCount >= kMinSamples; }

		// Median of what we have, clamped. 0 when there is nothing yet.
		double IntervalMs() const
		{
			if ( m_nCount == 0 )
				return 0.0;
			return ClampIntervalMs( MedianOf( m_flMs, m_nCount ) );
		}

		// Jitter of the recent intervals (SpreadOf), in milliseconds.
		double SpreadMs() const { return SpreadOf( m_flMs, m_nCount ); }

	private:
		double m_flMs[ kIntervalWindow ] = {};
		int m_nCount = 0;
		int m_nHead = 0;
	};

	// ------------------------------------------------------------------
	//  The pacer
	// ------------------------------------------------------------------

	// What layer 0 of the game looks like; a change is a D12 reset.
	struct LayerKey
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t format = 0;
		bool operator==( const LayerKey &o ) const { return width == o.width && height == o.height && format == o.format; }
		bool operator!=( const LayerKey &o ) const { return !( *this == o ); }
	};

	class Pacer
	{
	public:
		// Everything the glue reads fresh at each paint.
		struct Inputs
		{
			Ns     now = 0;            // the clock, for gaps, probes and status
			Ns     vblankNs = 0;       // V: the predicted present time of this composite (0 = now)
			Mode   mode = Mode::Fixed;
			int    multiplier = 2;     // Fixed: 2..kMaxN
			int    targetFps = 0;      // Target: 0 = the display's refresh, else 30..1000
			Priority priority = Priority::LowLatency;
			double refreshHz = 60.0;   // the refresh the vblank timer paces against
			float  estimateMs = -1.0f; // RenderStatus::lastEstimateMs, < 0 = n/a
			float  synthMs = -1.0f;    // RenderStatus::lastSynthMs, < 0 = n/a
			uint32_t costSeq = 0;      // RenderStatus::costSeq: changes with every new measurement
			bool   rendererOk = true;  // RenderStatus::reason == Ok
		};

		// What to do for this paint.
		struct Decision
		{
			uint64_t newestId = 0;     // the commit that is layer 0 (the renderer copies it on first sight)
			uint64_t prevId = 0;       // the pair a synth is made from (0 when showing the real frame)
			uint64_t currId = 0;
			uint64_t outId = 0;        // the output frame's identity: the same outId is the same image
			float    t = 1.0f;         // (0,1): synth at t between prevId and currId; 1: the real frame newestId
			bool inert = false;        // the renderer does nothing at all (not even the ring copy)
			bool reset = false;        // call fghost::Reset() BEFORE this composite
			bool repaintNext = false;  // another output is due: force a repaint on the next vblank
			bool newOutput = false;    // this paint presents a new output frame (counted in presentedFps)
		};

		// D18, in the glue's terms.
		struct Report
		{
			float  gameFps = 0.0f;
			float  presentedFps = 0.0f;
			float  effectiveMultiplier = 0.0f; // presented / game while generating
			float  targetFps = 0.0f;           // Target mode: the rate aimed for (clamped to the refresh), else 0
			int    chosenN = 0;                // Fixed: the multiplier; Target: 0
			int    activeN = 0;                // round(effective multiplier), >= 2 while generating, else 0
			float  delayMs = 0.0f;             // D
			Reason reason = Reason::Normal;
		};

		// Forget everything (frame generation was switched off/on, or a new
		// session). The next paint is "untracked" and starts clean.
		void Reset()
		{
			*this = Pacer();
		}

		// Layer 0 cannot take part right now (a fade, the Steam UI, no commit):
		// the previous real frames must not be used for the next pair.
		void Discontinuity()
		{
			DropHistory( Cause::Other, false );
			m_ulNewestId = 0;
			m_bPending = false;
		}

		// A real frame became the newest usable commit of the focused window.
		// "Arrival" = the earliest point at which it could be displayed: the
		// commit's acquire fence signalling (commit_t::Signal(), stamped in
		// commit_t::present_time). tNs is that time.
		void OnArrival( uint64_t ulCommitId, Ns tNs, uint64_t ulFocusKey, const LayerKey &layer )
		{
			m_flLastIntervalMs = 0.0;
			if ( m_bHaveFocus && ( ulFocusKey != m_ulFocusKey || layer != m_Layer ) )
			{
				// D12: another window, or the same one at another size / format.
				DropHistory( Cause::Focus, true );
			}
			else if ( m_bHaveArrival && tNs > m_ulLastArrival + kGapResetNs )
			{
				// D12: a gap in the game's frames.
				DropHistory( Cause::Gap, false );
			}
			else if ( m_bHaveArrival )
			{
				const Ns ulInterval = tNs >= m_ulLastArrival ? tNs - m_ulLastArrival : 0;
				m_Est.AddInterval( ulInterval );
				m_flLastIntervalMs = ClampIntervalMs( double( ulInterval ) / double( kNsPerMs ) );
			}

			m_ulFocusKey = ulFocusKey;
			m_Layer = layer;
			m_bHaveFocus = true;
			m_ulLastArrival = tNs;
			m_bHaveArrival = true;
			m_ulNewestId = ulCommitId;
			m_bPending = true;
		}

		// A paint is about to happen. `ulLayer0Id` is the commit that will be layer 0.
		// `bVblank` says whether it is the display's refresh that triggered it: on
		// a fixed-refresh display every paint is, and each one may consume one
		// output frame. A paint that is NOT a vblank only advances when a new real
		// frame is waiting; otherwise it repeats what is being shown.
		Decision OnPaint( const Inputs &in, uint64_t ulLayer0Id, bool bVblank )
		{
			if ( ulLayer0Id != m_ulNewestId )
			{
				// Layer 0 changed behind our back (focus switched with no arrival of
				// its own, or this is the first paint): no interval, no previous frame.
				DropHistory( Cause::Focus, true );
				m_ulNewestId = ulLayer0Id;
				m_bPending = true;
				m_bHaveArrival = false;
			}

			const bool bFresh = m_bPending;
			m_bPending = false;

			Evaluate( in );

			Decision d;
			d.newestId = ulLayer0Id;
			d.reset = m_bNeedReset;
			if ( d.reset )
				m_bNeedReset = false;

			switch ( m_Plan.state )
			{
				case State::Pass:
					ClearPlayState();
					d.t = 1.0f;
					d.inert = m_Plan.inert;
					d.newOutput = bFresh;
					m_flDelayMs = 0.0;
					break;

				case State::Warming:
					// Ask for the ring copy so the first pair after warm-up has its
					// previous frame.
					PushNewest( ulLayer0Id );
					d.t = 1.0f;
					d.newOutput = bFresh;
					m_flDelayMs = 0.0;
					break;

				case State::Generate:
					PaintGenerated( in, d, bFresh, bVblank );
					break;
			}

			if ( d.newOutput )
				m_nOutputs++;
			if ( m_Plan.state == State::Generate )
				m_Last = d;
			return d;
		}

		bool StatusDue( Ns ulNow ) const
		{
			return !m_bStatusStarted || ulNow - m_ulStatusAt >= kStatusPeriodNs;
		}

		// D18. Resets the presented-frames window, so call it only when it is going
		// to be published.
		Report TakeStatus( const Inputs &in, Ns ulNow )
		{
			Report s;
			s.chosenN = in.mode == Mode::Fixed ? in.multiplier : 0;

			const bool bEst = m_Est.Valid();
			if ( bEst )
				s.gameFps = float( 1000.0 / m_Est.IntervalMs() );

			if ( m_bStatusStarted && ulNow > m_ulStatusAt )
				s.presentedFps = float( double( m_nOutputs ) * 1e9 / double( ulNow - m_ulStatusAt ) );
			m_nOutputs = 0;
			m_ulStatusAt = ulNow;
			m_bStatusStarted = true;

			if ( in.mode == Mode::Target )
			{
				const double flRefresh = std::max( in.refreshHz, 1.0 );
				s.targetFps = float( in.targetFps > 0 ? std::min( double( in.targetFps ), flRefresh ) : flRefresh );
			}

			if ( !in.rendererOk )
			{
				s.reason = Reason::RendererUnavailable;
			}
			else if ( m_bHaveArrival && ulNow > m_ulLastArrival + kGapResetNs )
			{
				s.reason = Reason::GameStalled;
			}
			else if ( !bEst || m_Plan.state == State::Warming )
			{
				s.reason = ( m_Cause == Cause::Gap ) ? Reason::GameStalled : Reason::WarmingUp;
			}
			else
			{
				s.reason = m_Plan.reason;
				if ( m_Plan.state == State::Generate )
				{
					s.delayMs = float( m_flDelayMs );
					if ( s.gameFps > 0.0f )
					{
						s.effectiveMultiplier = s.presentedFps / s.gameFps;
						s.activeN = std::max( 2, int( std::lround( s.effectiveMultiplier ) ) );
					}
				}
			}
			return s;
		}

		// --- read-only views, for tests and the glue ---
		bool   EstimateValid() const { return m_Est.Valid(); }
		double IntervalMs() const { return m_Est.IntervalMs(); }
		bool   Generating() const { return m_Plan.state == State::Generate; }
		bool   CostBlocked() const { return m_bCostBlocked; }
		// The effective output interval (after the cost guard), ms; 0 when not generating.
		double OutputMs() const { return m_Plan.state == State::Generate ? m_Plan.oMs : 0.0; }
		double DelayMs() const { return m_flDelayMs; }
		int    HistoryCount() const { return m_nHist; }
		// The content time of the last output frame (tau, ns; 0 before any).
		int64_t ContentNs() const { return m_bTauValid ? m_nTauLast : 0; }

	private:
		enum class Cause : uint8_t { Unknown, Focus, Gap, Other };
		enum class State : uint8_t { Warming, Pass, Generate };

		struct Plan
		{
			State  state = State::Warming;
			Reason reason = Reason::WarmingUp;
			bool   inert = false;   // Pass only: the renderer stays completely inert
			double oMs = 0.0;       // Generate: the effective output interval
		};

		struct HistFrame
		{
			uint64_t id = 0;
			int64_t  arrival = 0;   // ns
		};

		// D12: forget the previous real frames and the interval history. The
		// renderer is told through Decision::reset at the next paint.
		void DropHistory( Cause cause, bool bNewGame )
		{
			m_Est.Clear();
			m_flLastIntervalMs = 0.0;
			ClearPlayState();
			m_bNeedReset = true;
			m_Cause = cause;
			if ( bNewGame )
			{
				// Another game / window: its rate and its cost are unrelated to the
				// last one's.
				m_bGenInit = false;
				m_bCostBlocked = false;
				m_bProbing = false;
			}
		}

		// Everything about the output sequence in progress (the content time, the
		// cadence phase, the frames kept).
		void ClearPlayState()
		{
			m_nHist = 0;
			m_bTauValid = false;
			m_nNextDue = 0;
			m_bExpectPaint = false;
			m_bHavePair = false;
			m_nPairSynths = 0;
			m_Last = Decision();
		}

		void PushNewest( uint64_t ulId )
		{
			if ( m_nHist > 0 && m_Hist[ m_nHist - 1 ].id == ulId )
				return;
			if ( m_nHist == kHistory )
			{
				for ( int i = 1; i < kHistory; i++ )
					m_Hist[ i - 1 ] = m_Hist[ i ];
				m_nHist--;
			}
			m_Hist[ m_nHist++ ] = HistFrame{ ulId, int64_t( m_ulLastArrival ) };
		}

		// Chooses what this paint does in general: pass-through (and why), warm-up,
		// or generation (and at what output interval).
		void Evaluate( const Inputs &in )
		{
			const uint32_t uKey = uint32_t( in.mode ) | ( uint32_t( in.multiplier ) << 2 ) |
				( uint32_t( in.targetFps ) << 8 ) | ( uint32_t( in.priority ) << 24 );
			if ( uKey != m_uCfgKey )
			{
				// The user picked something else: forget the content time and the
				// hysteresis memory and adopt the new target at once.
				m_uCfgKey = uKey;
				m_bGenInit = false;
				m_bTauValid = false;
				m_nNextDue = 0;
				m_bExpectPaint = false;
				m_Last = Decision();
			}

			Plan p;
			if ( !in.rendererOk )
			{
				// Keep telling the renderer (not inert): it only re-checks whether it
				// can run when it is driven, and it would otherwise stay "unavailable"
				// forever after the cause (HDR, ...) went away.
				p.state = State::Pass;
				p.reason = Reason::RendererUnavailable;
				p.inert = false;
				m_Plan = p;
				return;
			}
			if ( !m_Est.Valid() )
			{
				p.state = State::Warming;
				p.reason = Reason::WarmingUp;
				m_Plan = p;
				return;
			}

			const double flInterval = m_Est.IntervalMs();
			const double flBaseMs = OutputIntervalMs( in.mode, in.multiplier, in.targetFps, flInterval, in.refreshHz );
			const double flRatio = OutputRatio( flInterval, flBaseMs );

			// The game reaches the output rate: nothing to generate. Hysteresis is a
			// ratio band, not a time hold (the first decision after a start or a
			// change uses the stop ratio alone).
			if ( !m_bGenInit )
			{
				m_bGen = flRatio > kStopRatio;
				m_bGenInit = true;
			}
			else if ( m_bGen )
			{
				if ( flRatio < kStopRatio )
					m_bGen = false;
			}
			else if ( flRatio > kStartRatio )
			{
				m_bGen = true;
			}
			if ( !m_bGen )
			{
				p.state = State::Pass;
				p.reason = Reason::GameTooFast;
				p.inert = true;
				m_Plan = p;
				return;
			}

			double flOutMs = flBaseMs;
			Reason eReason = Reason::Normal;

			const bool bTimed = in.estimateMs >= 0.0f && in.synthMs >= 0.0f;
			if ( !bTimed )
			{
				m_bCostBlocked = false;
				m_bProbing = false;
			}
			else
			{
				if ( m_bCostBlocked && !m_bProbing && in.now >= m_ulCostBlockedAt + kCostProbeNs )
				{
					m_bProbing = true;
					m_ulProbeStart = in.now;
					m_uProbeSeq = in.costSeq;
				}
				if ( m_bProbing )
				{
					if ( in.costSeq != m_uProbeSeq )
					{
						// A measurement of a pair generated by the probe.
						m_bProbing = false;
						if ( MaxSynthsPerPair( flInterval, in.estimateMs, in.synthMs ) >= 1 )
							m_bCostBlocked = false;
						else
							m_ulCostBlockedAt = in.now;
					}
					else if ( in.now >= m_ulProbeStart + kCostProbeMaxNs )
					{
						// Nothing measured: stay blocked, try again later.
						m_bProbing = false;
						m_ulCostBlockedAt = in.now;
					}
				}

				if ( m_bCostBlocked )
				{
					if ( !m_bProbing )
					{
						p.state = State::Pass;
						p.reason = Reason::CostGuard;
						p.inert = true;
						m_Plan = p;
						return;
					}
					// The probe: one generated frame per real frame.
					flOutMs = std::max( flOutMs, flInterval * 0.5 );
					eReason = Reason::CostGuard;
				}
				else
				{
					const int nMax = MaxSynthsPerPair( flInterval, in.estimateMs, in.synthMs );
					if ( nMax < 1 )
					{
						m_bCostBlocked = true;
						m_ulCostBlockedAt = in.now;
						p.state = State::Pass;
						p.reason = Reason::CostGuard;
						p.inert = true;
						m_Plan = p;
						return;
					}
					const double flCostMs = flInterval / double( nMax + 1 );
					if ( flCostMs > flOutMs + 1e-9 )
					{
						flOutMs = flCostMs;
						eReason = Reason::CostGuard;
					}
				}
			}

			p.state = State::Generate;
			p.reason = eReason;
			p.oMs = flOutMs;
			m_Plan = p;
		}

		// The delay D for the current plan, ms.
		double DelayFor( const Inputs &in ) const
		{
			const double flInterval = m_Est.IntervalMs();
			if ( in.priority == Priority::Smoothness )
				return SmoothnessDelayMs( flInterval, m_Est.SpreadMs() );
			const double flLatest = m_flLastIntervalMs > 0.0 ? m_flLastIntervalMs : flInterval;
			return LowLatencyDelayMs( flLatest, m_Plan.oMs );
		}

		void Repeat( Decision &d, const Decision &last, bool bRepaintNext ) const
		{
			const bool bReset = d.reset;
			const uint64_t ulNewest = d.newestId;
			d = last;
			d.newestId = ulNewest;
			d.reset = bReset;
			d.newOutput = false;
			d.repaintNext = bRepaintNext;
		}

		void PaintGenerated( const Inputs &in, Decision &d, bool bFresh, bool bVblank )
		{
			PushNewest( d.newestId );

			if ( m_nHist < 2 )
			{
				// First frame after a reset / a pass-through: it is only copied;
				// generation starts with the next one.
				d.t = 1.0f;
				d.newOutput = bFresh;
				m_flDelayMs = 0.0;
				return;
			}

			const bool bExpect = bFresh || m_bExpectPaint;
			if ( !bExpect || ( !bVblank && !bFresh ) )
			{
				// Nothing new to show (a UI / cursor repaint): the same output again.
				Repeat( d, m_Last, m_bExpectPaint );
				return;
			}

			const int64_t V = int64_t( in.vblankNs ? in.vblankNs : in.now );
			const double flVblankMs = 1000.0 / std::max( in.refreshHz, 1.0 );
			const double flOutMs = m_Plan.oMs;
			const bool bCadence = flOutMs > flVblankMs * 1.02;

			// Output cadence: a vblank that does not cross the output interval keeps
			// the previous image. A real frame held (t = 1) is not repeatable once
			// layer 0 has moved on, so only a cached synth may be skipped over.
			const bool bLastSynth = m_Last.t < 1.0f && m_Last.outId != 0;
			if ( bCadence )
			{
				const int64_t nVblankNs = int64_t( flVblankMs * 1e6 );
				const int64_t nOutNs = int64_t( flOutMs * 1e6 );
				const bool bDue = m_nNextDue != 0 && V + nVblankNs / 2 >= m_nNextDue;
				if ( m_nNextDue != 0 && !bDue && bLastSynth )
				{
					Repeat( d, m_Last, true );
					return;
				}
				// A due vblank advances the accumulator by one output interval, but
				// never lets it lag more than one vblank behind (slots that lapsed
				// while holding a real frame are not made up for with a burst). An
				// output that was not due (a new real frame after a held one) starts
				// the phase afresh.
				if ( bDue )
					m_nNextDue = std::max( m_nNextDue + nOutNs, V + nOutNs - nVblankNs );
				else
					m_nNextDue = V + nOutNs;
			}
			else
			{
				m_nNextDue = 0;
			}

			// Content time: tau = V - D, never older than the last output's.
			const double flDelayMs = DelayFor( in );
			m_flDelayMs = flDelayMs;
			int64_t tau = V - int64_t( flDelayMs * 1e6 );
			if ( m_bTauValid && tau < m_nTauLast )
				tau = m_nTauLast;
			m_nTauLast = tau;
			m_bTauValid = true;

			const HistFrame &newest = m_Hist[ m_nHist - 1 ];
			d.newOutput = true;

			// Low latency plays only the newest pair (a new real frame snaps the
			// content time to it); Smoothness may still be inside an older one.
			const int jMin = in.priority == Priority::LowLatency ? m_nHist - 2 : 0;
			bool bReal = tau >= newest.arrival;
			int j = m_nHist - 2;
			double t = 1.0;
			if ( !bReal )
			{
				while ( j > jMin && m_Hist[ j ].arrival > tau )
					j--;
				const int64_t nSpan = std::max<int64_t>( m_Hist[ j + 1 ].arrival - m_Hist[ j ].arrival, 1 );
				t = double( tau - m_Hist[ j ].arrival ) / double( nSpan );
				if ( j + 1 == m_nHist - 1 && t >= kRealSnapT )
					bReal = true;
				else
					t = std::min( std::max( t, kTMin ), kRealSnapT );
			}

			if ( bReal )
			{
				// The real frame itself: nothing more is due until a newer one arrives.
				d.t = 1.0f;
				d.prevId = d.currId = 0;
				d.outId = ++m_ulOutSeq;
				d.repaintNext = false;
				m_bExpectPaint = false;
				m_bHavePair = false;
				return;
			}

			d.prevId = m_Hist[ j ].id;
			d.currId = m_Hist[ j + 1 ].id;
			d.t = float( t );

			if ( m_bHavePair && m_PairPrev == d.prevId && m_PairCurr == d.currId )
			{
				m_nPairSynths++;
			}
			else
			{
				m_bHavePair = true;
				m_PairPrev = d.prevId;
				m_PairCurr = d.currId;
				m_nPairSynths = 1;
			}

			// The same instant of the same pair needs no second synth (a clamped
			// content time), and a pair at its synth cap repeats its last one.
			const bool bSamePair = bLastSynth && m_Last.prevId == d.prevId && m_Last.currId == d.currId;
			if ( bSamePair && ( m_nPairSynths > kMaxSynthsPerPair || std::fabs( m_Last.t - d.t ) < 1e-4f ) )
			{
				d.t = m_Last.t;
				d.outId = m_Last.outId;
				m_nPairSynths = std::min( m_nPairSynths, kMaxSynthsPerPair );
			}
			else
			{
				d.outId = ++m_ulOutSeq;
			}
			d.repaintNext = true;
			m_bExpectPaint = true;
		}

		// -- arrivals / geometry --
		uint64_t m_ulNewestId = 0;
		bool     m_bPending = false;         // the newest arrival has not been painted yet
		bool     m_bHaveArrival = false;
		Ns       m_ulLastArrival = 0;
		double   m_flLastIntervalMs = 0.0;   // the most recent real interval (0 = none yet)
		bool     m_bHaveFocus = false;
		uint64_t m_ulFocusKey = 0;
		LayerKey m_Layer;
		IntervalEstimator m_Est;

		// -- the output sequence in progress --
		HistFrame m_Hist[ kHistory ];
		int      m_nHist = 0;
		bool     m_bTauValid = false;
		int64_t  m_nTauLast = 0;
		int64_t  m_nNextDue = 0;             // the cadence accumulator, ns (0 = none)
		bool     m_bExpectPaint = false;     // we asked for the next vblank's repaint
		bool     m_bHavePair = false;
		uint64_t m_PairPrev = 0, m_PairCurr = 0;
		int      m_nPairSynths = 0;
		uint64_t m_ulOutSeq = 0;
		bool     m_bNeedReset = false;
		double   m_flDelayMs = 0.0;
		Decision m_Last;
		Cause    m_Cause = Cause::Unknown;

		// -- the plan --
		Plan     m_Plan;
		uint32_t m_uCfgKey = 0xFFFFFFFFu;
		bool     m_bGenInit = false;
		bool     m_bGen = false;

		// -- cost guard --
		bool     m_bCostBlocked = false;
		Ns       m_ulCostBlockedAt = 0;
		bool     m_bProbing = false;
		Ns       m_ulProbeStart = 0;
		uint32_t m_uProbeSeq = 0;

		// -- status --
		uint64_t m_nOutputs = 0;
		bool     m_bStatusStarted = false;
		Ns       m_ulStatusAt = 0;
	};
}
