#pragma once

// Pacing.h -- the pure decision logic of frame-generation pacing.
//
// Header-only, no gamescope globals, no Vulkan, no clock of its own: every
// time is passed in (nanoseconds, any monotonic base), so tests/test_framegen_
// pacing.cpp drives it with a fake clock. steamcompmgr.cpp is the thin glue
// that feeds it commit arrivals and paints and takes back (k, n).
//
// WHAT IT DECIDES (superdoc/planning/fidelityfx-opticalflow-framegen.md 5.1/5.2
// and the FG decisions D2..D21 in the plan):
//   * which image each display refresh shows: generated slot k of N, or the
//     real frame (the "slot sequencer", Pacer::OnPaint);
//   * what N is actually usable (the game's rate vs the display's, the cost
//     guard) and when it may change (the 0.5 s hysteresis);
//   * when to forget the previous real frame (focus / size / format / gap).
//
// THE MODEL (fixed refresh, one image per vblank). A "pair" is N-1 generated
// frames at t = k/N followed by the real frame they lead up to. When a real
// frame arrives, the NEXT vblank shows slot 1, the one after slot 2 ... and the
// vblank after the last generated slot shows the real frame itself. So a real
// frame is held back by N-1 refreshes, which is the price of generation.
//   Why: generated frames lie BETWEEN the previous and the newest real frame,
//   so they can only be made once the newest exists, and they must be shown
//   before it.
//
// LATENCY FIRST (D9). A real frame is never held past its own slot: if a newer
// real frame arrives while a pair is still playing out, what is left of that
// pair (its remaining generated frames and its held real frame) is DROPPED and
// the new pair starts at once. Delay therefore cannot pile up.
//   Why: the alternative (finish the old pair first) would grow a queue
//   whenever the game briefly runs above refresh/N, and latency is what the
//   user is least willing to give up.
//
// Phase A is vblank-driven for every backend. "The next vblank" is the only
// place the display's clock enters (Pacer::OnPaint), so a later phase can
// replace it with a per-slot timer without touching anything else here.

#include <algorithm>
#include <cmath>
#include <cstdint>

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
	// D10: a pass-through / N change happens only after the target has held this long.
	constexpr Ns     kHysteresisNs    = 500 * kNsPerMs;
	// D5: "fits" = game fps x N <= kFitHeadroom x refresh. The 20% headroom lets a
	// game hover a little above refresh/N and lose the odd real frame to D9
	// rather than switching down at the threshold.
	constexpr double kFitHeadroom     = 1.2;
	// D16: generation (estimate + synths) may use at most this share of a game
	// interval.
	constexpr double kCostBudget      = 0.25;
	// After a cost-guard pass-through nothing is generated, hence nothing is
	// measured; retry N = 2 this often to see whether the load has gone.
	constexpr Ns     kCostProbeNs     = 10'000'000'000ull;
	// Status is worth publishing this often (D18).
	constexpr Ns     kStatusPeriodNs  = 250 * kNsPerMs;

	// The largest multiplier the host supports (FrameGenHost.h kMaxMultiplier).
	constexpr int    kMaxN            = 4;

	// Mirrors fghost::PassReason value for value (steamcompmgr.cpp static_asserts
	// it); duplicated so this header stays free of the host's includes.
	enum class Reason : uint8_t
	{
		Normal,
		Off,
		WarmingUp,
		RefreshLimit,
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

	// Median of v[0..n): the middle value, or the mean of the two middle ones
	// for an even count. n is tiny (<= 8), so a copy + insertion sort.
	inline double MedianOf( const double *v, int n )
	{
		if ( n <= 0 )
			return 0.0;
		double s[ kIntervalWindow ];
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
		return ( n & 1 ) ? s[ n / 2 ] : 0.5 * ( s[ n / 2 - 1 ] + s[ n / 2 ] );
	}

	// D5: can N x the game's rate be shown on this display?
	inline bool Fits( double flIntervalMs, int n, double flRefreshHz )
	{
		if ( flIntervalMs <= 0.0 || flRefreshHz <= 0.0 )
			return false;
		const double flGameFps = 1000.0 / flIntervalMs;
		// 1e-9: 0.6 x 144 must fit 86.4 exactly, not lose to rounding.
		return flGameFps * n <= kFitHeadroom * flRefreshHz + 1e-9;
	}

	// What the user's "N" resolves to right now, before hysteresis.
	struct NChoice
	{
		int    n      = 0;                 // 0 = pass-through, else 2..kMaxN
		Reason reason = Reason::Normal;    // why n < chosen (Normal when n == chosen)
	};

	// D21: the highest N' <= chosenN that fits the display (D5) and passes the cost
	// guard (D16); below 2 it is pass-through.
	//   flCostPerNMs : measured generation cost per unit of N (cost of the last
	//                  pair divided by the N it ran at), or < 0 when unknown /
	//                  the GPU cannot time it -- then there is no guard at all.
	//   bIgnoreCost  : the cost probe (see kCostProbeNs): trust nothing stale,
	//                  try N = 2.
	// Why a per-N model: the renderer reports one number for the whole pair
	// (estimate + synths) and we cannot split it, so cost(N') is assumed to scale
	// with N'. Conservative for the (fixed) estimate part, which is the safe side
	// for a guard.
	inline NChoice ChooseN( int nChosen, double flIntervalMs, double flRefreshHz, double flCostPerNMs, bool bIgnoreCost = false )
	{
		NChoice c;
		nChosen = std::min( nChosen, kMaxN );
		if ( nChosen < 2 )
		{
			c.reason = Reason::Off;
			return c;
		}

		int n = nChosen;
		while ( n >= 2 && !Fits( flIntervalMs, n, flRefreshHz ) )
			n--;
		if ( n < 2 )
		{
			c.n = 0;
			c.reason = Reason::GameTooFast;
			return c;
		}
		c.n = n;
		c.reason = ( n < nChosen ) ? Reason::RefreshLimit : Reason::Normal;

		if ( bIgnoreCost )
		{
			c.n = std::min( c.n, 2 );
			return c;
		}
		if ( flCostPerNMs >= 0.0 )
		{
			const double flBudgetMs = kCostBudget * flIntervalMs;
			bool bStepped = false;
			while ( c.n >= 2 && flCostPerNMs * c.n > flBudgetMs )
			{
				c.n--;
				bStepped = true;
			}
			if ( c.n < 2 )
			{
				c.n = 0;
				c.reason = Reason::CostGuard;
			}
			else if ( bStepped )
			{
				c.reason = Reason::CostGuard;
			}
		}
		return c;
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

	private:
		double m_flMs[ kIntervalWindow ] = {};
		int m_nCount = 0;
		int m_nHead = 0;
	};

	// ------------------------------------------------------------------
	//  Hysteresis (D10)
	// ------------------------------------------------------------------

	// Holds the multiplier in use. A new target (including pass-through) has to be
	// the target continuously for kHysteresisNs before it replaces the held one,
	// so a game hovering at a threshold does not flap.
	class NHysteresis
	{
	public:
		// The next Update() adopts its target at once (first estimate, a new
		// game, the user picking a different multiplier).
		void Restart() { m_bInit = false; m_bPending = false; }

		const NChoice &Update( Ns ulNow, const NChoice &target )
		{
			if ( !m_bInit )
			{
				m_bInit = true;
				m_Held = target;
				m_bPending = false;
				m_bChanged = true;
				return m_Held;
			}
			m_bChanged = false;

			if ( target.n == m_Held.n )
			{
				m_Held.reason = target.reason;   // same N, maybe a better explanation
				m_bPending = false;
				return m_Held;
			}
			if ( !m_bPending || target.n != m_PendingN )
			{
				m_bPending = true;
				m_PendingN = target.n;
				m_ulSince = ulNow;
				return m_Held;
			}
			if ( ulNow - m_ulSince >= kHysteresisNs )
			{
				m_Held = target;
				m_bPending = false;
				m_bChanged = true;
			}
			return m_Held;
		}

		const NChoice &Held() const { return m_Held; }
		bool Initialised() const { return m_bInit; }
		// True for the Update() that changed (or first set) the held value.
		bool Changed() const { return m_bChanged; }

	private:
		NChoice m_Held;
		bool m_bInit = false;
		bool m_bPending = false;
		bool m_bChanged = false;
		int  m_PendingN = 0;
		Ns   m_ulSince = 0;
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
			Ns     now = 0;
			int    chosenN = 0;        // the user's multiplier (fghost::GetConfig().multiplier)
			double refreshHz = 60.0;   // the refresh the vblank timer paces against
			float  costMs = -1.0f;     // RenderStatus::lastPairGpuMs, < 0 = n/a
			bool   rendererOk = true;  // RenderStatus::reason == Ok
		};

		// What to do for this paint.
		struct Decision
		{
			uint64_t pairId = 0;       // fghost::SetSlot's pairId: the commit that is layer 0
			int  k = 0;                // 1..n-1: generated slot k of n.  0: the real frame
			int  n = 0;                // fghost::SetSlot's n.  0: renderer inert (not even the ring copy)
			bool reset = false;        // call fghost::Reset() BEFORE this composite
			bool repaintNext = false;  // the pair is not finished: force a repaint on the next vblank
		};

		// D18, in the glue's terms.
		struct Report
		{
			float  gameFps = 0.0f;
			float  presentedFps = 0.0f;
			int    chosenN = 0;
			int    activeN = 0;
			float  delayMs = 0.0f;
			Reason reason = Reason::Normal;
		};

		// Forget everything (frame generation was switched off/on, or a new
		// session). The next paint is "untracked" and starts clean.
		void Reset()
		{
			*this = Pacer();
		}

		// Layer 0 cannot take part right now (a fade, the Steam UI, no commit):
		// the previous real frame must not be used for the next pair.
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
				m_Est.AddInterval( tNs >= m_ulLastArrival ? tNs - m_ulLastArrival : 0 );
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
		// a fixed-refresh display every paint is, and each one consumes one slot.
		// A paint that is NOT a vblank (VRR / tearing paint straight on a commit)
		// only advances the sequence when a new real frame is waiting; otherwise it
		// repeats what is being shown.
		Decision OnPaint( const Inputs &in, uint64_t ulLayer0Id, bool bVblank )
		{
			m_nPaints++;

			if ( ulLayer0Id != m_ulNewestId )
			{
				// Layer 0 changed behind our back (focus switched with no arrival of
				// its own, or this is the first paint): no interval, no previous frame.
				DropHistory( Cause::Focus, true );
				m_ulNewestId = ulLayer0Id;
				m_bPending = true;
				m_bHaveArrival = false;
			}

			UpdateTarget( in );

			Decision d;
			d.pairId = ulLayer0Id;

			if ( m_bPending )
			{
				// D9: whatever is left of an older pair is dropped; this one starts now.
				StartPair( in, d );
			}
			else if ( !bVblank )
			{
				d = m_Last;
				d.reset = false;
				d.repaintNext = m_bPairActive;
			}
			else if ( m_bPairActive )
			{
				ContinuePair( d );
			}
			else
			{
				// Nothing new (a UI repaint): the real frame again. Same slot as last
				// time, so the renderer reuses what it has.
				d.k = 0;
				d.n = m_nIdleN;
			}

			if ( d.reset )
				m_bNeedReset = false;
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
			s.chosenN = in.chosenN;

			const bool bEst = m_Est.Valid();
			if ( bEst )
				s.gameFps = float( 1000.0 / m_Est.IntervalMs() );

			if ( m_bStatusStarted && ulNow > m_ulStatusAt )
				s.presentedFps = float( double( m_nPaints ) * 1e9 / double( ulNow - m_ulStatusAt ) );
			m_nPaints = 0;
			m_ulStatusAt = ulNow;
			m_bStatusStarted = true;

			if ( !in.rendererOk )
			{
				s.reason = Reason::RendererUnavailable;
			}
			else if ( m_bHaveArrival && ulNow > m_ulLastArrival + kGapResetNs )
			{
				s.reason = Reason::GameStalled;
			}
			else if ( !bEst || !m_Hyst.Initialised() )
			{
				s.reason = ( m_Cause == Cause::Gap ) ? Reason::GameStalled : Reason::WarmingUp;
			}
			else
			{
				s.reason = m_Hyst.Held().reason;
				s.activeN = m_Hyst.Held().n;
			}

			if ( s.activeN >= 2 )
				s.delayMs = float( double( s.activeN - 1 ) / double( s.activeN ) * m_Est.IntervalMs() );
			return s;
		}

		// --- read-only views, for tests and the glue ---
		bool   EstimateValid() const { return m_Est.Valid(); }
		double IntervalMs() const { return m_Est.IntervalMs(); }
		int    ActiveN() const { return m_Hyst.Held().n; }
		bool   PairActive() const { return m_bPairActive; }
		bool   HavePrev() const { return m_bHavePrev; }

	private:
		enum class Cause : uint8_t { Unknown, Focus, Gap, Other };

		// D12: forget the previous real frame and the interval history. The
		// renderer is told through Decision::reset at the next paint.
		void DropHistory( Cause cause, bool bNewGame )
		{
			m_Est.Clear();
			m_bHavePrev = false;
			m_bPairActive = false;
			m_bNeedReset = true;
			m_Cause = cause;
			if ( bNewGame )
			{
				// Another game / window: its rate and its cost are unrelated to the
				// last one's, so adopt the new target at once and unlearn the cost.
				m_Hyst.Restart();
				m_flCostPerNMs = -1.0;
				m_bCostBlocked = false;
				m_nLastGenN = 0;
				m_nSettle = 3;
			}
		}

		void UpdateTarget( const Inputs &in )
		{
			if ( in.chosenN != m_nChosen )
			{
				m_nChosen = in.chosenN;
				m_Hyst.Restart();
			}

			// Cost model (D16). Only pairs that really generated feed it, and not
			// for a few pairs after N changed: the renderer's number lags the live
			// pair by up to one, so right after a change it still belongs to the
			// old N.
			if ( in.costMs >= 0.0f && m_nLastGenN >= 2 && m_nSettle == 0 )
				m_flCostPerNMs = double( in.costMs ) / double( m_nLastGenN );
			else if ( in.costMs < 0.0f )
				m_flCostPerNMs = -1.0;

			if ( !m_Est.Valid() || !in.rendererOk )
				return;

			const double flInterval = m_Est.IntervalMs();
			const bool bProbe = m_bCostBlocked && m_Hyst.Held().n < 2 && in.now >= m_ulCostBlockedAt + kCostProbeNs;
			const NChoice target = ChooseN( in.chosenN, flInterval, in.refreshHz, m_flCostPerNMs, bProbe );

			const NChoice &held = m_Hyst.Update( in.now, target );
			if ( m_Hyst.Changed() )
			{
				m_nSettle = 3;
				if ( held.n < 2 && held.reason == Reason::CostGuard )
				{
					m_bCostBlocked = true;
					m_ulCostBlockedAt = in.now;
				}
				else if ( held.n >= 2 )
				{
					m_bCostBlocked = false;
				}
			}
		}

		// The first paint of a pair: the real frame has arrived.
		void StartPair( const Inputs &in, Decision &d )
		{
			m_bPending = false;
			m_bPairActive = false;
			d.reset = m_bNeedReset;

			const NChoice &held = m_Hyst.Held();

			if ( !in.rendererOk )
			{
				// Keep telling the renderer (n >= 2): it only re-checks whether it can
				// run when it is driven, and it would otherwise stay "unavailable"
				// forever after the cause (HDR, ...) went away.
				d.k = 0;
				d.n = std::max( in.chosenN, 2 );
				m_bHavePrev = false;
				m_nIdleN = d.n;
				return;
			}
			if ( !m_Est.Valid() )
			{
				// Warming up (a new game, or after a gap). Ask for the ring copy so the
				// first pair after it has its previous frame.
				d.k = 0;
				d.n = std::max( in.chosenN, 2 );
				m_bHavePrev = true;
				m_nIdleN = d.n;
				return;
			}
			if ( held.n < 2 )
			{
				// Settled pass-through: the real frame at once, the renderer left
				// completely inert (no copy either).
				d.k = 0;
				d.n = 0;
				m_bHavePrev = false;
				m_nIdleN = 0;
				return;
			}
			if ( !m_bHavePrev )
			{
				// First pair after a reset / a pass-through: this frame is only
				// copied; generation starts with the next one.
				d.k = 0;
				d.n = held.n;
				m_bHavePrev = true;
				m_nIdleN = held.n;
				return;
			}

			// A pair: slot 1 now, slots 2..n-1 and then the real frame on the
			// following refreshes. N is latched here: a change takes effect only at a
			// pair boundary.
			d.k = 1;
			d.n = held.n;
			d.repaintNext = true;
			m_bPairActive = true;
			m_nPairN = held.n;
			m_nNextK = 2;
			m_nIdleN = held.n;
			m_nLastGenN = held.n;
			if ( m_nSettle > 0 )
				m_nSettle--;
		}

		void ContinuePair( Decision &d )
		{
			d.n = m_nPairN;
			if ( m_nNextK < m_nPairN )
			{
				d.k = m_nNextK++;
				d.repaintNext = true;
			}
			else
			{
				// The generated frames are out: now the real one.
				d.k = 0;
				m_bPairActive = false;
			}
		}

		// -- arrivals / geometry --
		uint64_t m_ulNewestId = 0;
		bool     m_bPending = false;         // the newest arrival has not started a pair yet
		bool     m_bHaveArrival = false;
		Ns       m_ulLastArrival = 0;
		bool     m_bHaveFocus = false;
		uint64_t m_ulFocusKey = 0;
		LayerKey m_Layer;
		IntervalEstimator m_Est;

		// -- the pair being played out --
		bool m_bPairActive = false;
		int  m_nPairN = 0;
		int  m_nNextK = 0;
		bool m_bHavePrev = false;            // the renderer has the previous real frame
		bool m_bNeedReset = false;
		int  m_nIdleN = 0;                   // the n to repeat while nothing new arrives
		Decision m_Last;
		Cause m_Cause = Cause::Unknown;

		// -- N selection --
		NHysteresis m_Hyst;
		int    m_nChosen = 0;
		double m_flCostPerNMs = -1.0;
		int    m_nLastGenN = 0;
		int    m_nSettle = 0;
		bool   m_bCostBlocked = false;
		Ns     m_ulCostBlockedAt = 0;

		// -- status --
		uint64_t m_nPaints = 0;
		bool     m_bStatusStarted = false;
		Ns       m_ulStatusAt = 0;
	};
}
