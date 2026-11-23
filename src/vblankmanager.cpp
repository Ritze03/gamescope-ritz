// Try to figure out when vblank is and notify steamcompmgr to render some time before it

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <condition_variable>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "gpuvis_trace_utils.h"

#include "vblankmanager.hpp"
#include "steamcompmgr.hpp"
#include "main.hpp"
#include "refresh_rate.h"

LogScope g_VBlankLog("vblank");

namespace gamescope
{
	ConVar<bool> vblank_debug( "vblank_debug", false, "Enable vblank debug spew to stderr." );
	ConVar<bool> vblank_measured_lead( "vblank_measured_lead", true, "Wake up for a vblank by the measured draw time (largest of the last 60 draws + 0.75 ms, at least 1.5 ms) instead of the fixed-ish red zone heuristic. Mirrors the Display > General \"Low-latency wake-up\" setting." );

	CVBlankTimer::CVBlankTimer()
	{
		m_ulTargetVBlank = get_time_in_nanos();
		m_ulLastVBlank = m_ulTargetVBlank;

		if ( !GetBackend()->NeedsFrameSync() )
		{
			// Majority of backends fall down this optimal
			// timerfd path, vs nudge thread.
			g_VBlankLog.infof( "Using timerfd." );
		}
		else
		{
			g_VBlankLog.infof( "Using nudge thread." );

			if ( pipe2( m_nNudgePipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
			{
				g_VBlankLog.errorf_errno( "Failed to create VBlankTimer pipe." );
				abort();
			}

			std::thread vblankThread( [this]() { this->NudgeThread(); } );
			vblankThread.detach();
		}
	}

	CVBlankTimer::~CVBlankTimer()
	{
		std::unique_lock lock( m_ScheduleMutex );

		m_bRunning = false;

		m_bArmed = true;
		m_bArmed.notify_all();

		for ( int i = 0; i < 2; i++ )
		{
			if ( m_nNudgePipe[ i ] >= 0 )
			{
				close ( m_nNudgePipe[ i ] );
				m_nNudgePipe[ i ] = -1;
			}
		}
	}

	int CVBlankTimer::GetRefresh() const
	{
		return g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh;
	}

	uint64_t CVBlankTimer::GetLastVBlank() const
	{
		return m_ulLastVBlank;
	}

	uint64_t CVBlankTimer::GetNextVBlank( uint64_t ulOffset ) const
	{
		const uint64_t ulIntervalNSecs = mHzToRefreshCycle( GetRefresh() );
		const uint64_t ulNow = get_time_in_nanos();

		uint64_t ulTargetPoint = GetLastVBlank() + ulIntervalNSecs - ulOffset;

		// Same result as stepping one refresh at a time, without a loop that grows with the
		// anchor's age (ritz_timer_feedback_anchor 4 never moves the anchor).
		if ( ulTargetPoint < ulNow && ulIntervalNSecs )
			ulTargetPoint += ( ( ulNow - ulTargetPoint + ulIntervalNSecs - 1 ) / ulIntervalNSecs ) * ulIntervalNSecs;

		return ulTargetPoint;
	}

	VBlankScheduleTime CVBlankTimer::CalcNextWakeupTime( bool bPreemptive )
	{
		// ponytail: GetBackend() can already be null here. The present-wait thread
		// that drives MarkVBlank()/ArmNextVBlank() on the Vulkan-swapchain path
		// (present_wait_thread_func, src/rendervulkan.cpp) is detached and keeps
		// running until the process actually exits, even after steamcompmgr's
		// shutdown path tears the backend down (IBackend::Set(nullptr),
		// src/steamcompmgr.cpp) -- a pre-existing shutdown race, reproduced here
		// independent of any Vulkan layer, that only bit reliably once something
		// (e.g. a present-hooking layer) stretches out the shutdown window. Treat
		// a torn-down backend the same way GetScreenType() already treats a
		// connector-less one: an internal screen with no VRR, rather than
		// dereferencing a null IBackend*.
		IBackend *pBackend = GetBackend();
		const GamescopeScreenType eScreenType = pBackend ? pBackend->GetScreenType() : GAMESCOPE_SCREEN_TYPE_INTERNAL;

		const int nRefreshRate = GetRefresh();
		const uint64_t ulRefreshInterval = mHzToRefreshCycle( nRefreshRate );

		bool bVRR = pBackend && pBackend->GetCurrentConnector() && pBackend->GetCurrentConnector()->IsVRRActive();
		uint64_t ulOffset = 0;
		LeadKind eKind = LeadKind::Default;
		if ( !bVRR && vblank_measured_lead )
		{
			// Low-latency wake-up (on by default): all the work of a frame (frame
			// generation, blur, the lag buffer, plain compositing) is inside the draw
			// we measure (wake-up -> present), so the lead only has to be what drawing
			// actually needs. The upstream heuristic below never goes under
			// 2.4 ms (compositing floor) + the red zone, which at 280 Hz is more than a
			// whole refresh: a real frame landing in that lead is shown one refresh late,
			// and the library's Low latency cannot fit generated frames between real ones.
			// A rolling MAX (not an average) so one slow draw widens the lead at once and
			// it narrows again only after kDrawTimeWindow clean ones.
			// Not under VRR: it was tried (2026-10-06) and reverted, the lead sat at the
			// full-refresh cap and raced the feedback-driven re-arm. VRR keeps upstream's.
			ulOffset = MeasuredLead( m_ulRecentMaxDrawTime.load( std::memory_order_relaxed ), ulRefreshInterval );
			eKind = LeadKind::Measured;

			if ( vblank_debug && !bPreemptive )
			{
				static uint64_t s_ulMeasuredSpew = 0;
				if ( s_ulMeasuredSpew++ % 300 == 0 )
					g_VBlankLog.infof( "measured lead %.2fms (max draw %.2fms of the last %zu)", ulOffset / 1'000'000.0,
						m_ulRecentMaxDrawTime.load( std::memory_order_relaxed ) / 1'000'000.0, kDrawTimeWindow );
			}
		}
		else if ( !bVRR )
		{
			// The redzone is relative to 60Hz for external displays.
			// Scale it by our target refresh so we don't miss submitting for
			// vblank in DRM.
			// (This fixes wonky frame-pacing on 4K@30Hz screens)
			//
			// TODO(Josh): Is this fudging still needed with our SteamOS kernel patches
			// to not account for vertical front porch when dealing with the vblank
			// drm_commit is going to target?
			// Need to re-test that.
			const uint64_t ulRedZone = eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL
				? m_ulVBlankDrawBufferRedZone
				: std::min<uint64_t>( m_ulVBlankDrawBufferRedZone, ( m_ulVBlankDrawBufferRedZone * 60'000 * nRefreshRate ) / 60'000 );

			const uint64_t ulDecayAlpha = m_ulVBlankRateOfDecayPercentage; // eg. 980 = 98%

			uint64_t ulDrawTime = m_ulLastDrawTime;
			/// See comment of m_ulVBlankDrawTimeMinCompositing.
			if ( m_bCurrentlyCompositing )
				ulDrawTime = std::max( ulDrawTime, m_ulVBlankDrawTimeMinCompositing );

			uint64_t ulNewRollingDrawTime;
			// This is a rolling average when ulDrawTime < m_ulRollingMaxDrawTime,
			// and a maximum when ulDrawTime > m_ulRollingMaxDrawTime.
			//
			// This allows us to deal with spikes in the draw buffer time very easily.
			// eg. if we suddenly spike up (eg. because of test commits taking a stupid long time),
			// we will then be able to deal with spikes in the long term, even if several commits after
			// we get back into a good state and then regress again.

			// If we go over half of our deadzone, be more defensive about things and
			// spike up back to our current drawtime (sawtooth).
			if ( int64_t( ulDrawTime ) - int64_t( ulRedZone / 2 ) > int64_t( m_ulRollingMaxDrawTime ) )
				ulNewRollingDrawTime = ulDrawTime;
			else
				ulNewRollingDrawTime = ( ( ulDecayAlpha * m_ulRollingMaxDrawTime ) + ( kVBlankRateOfDecayMax - ulDecayAlpha ) * ulDrawTime ) / kVBlankRateOfDecayMax;

			// If we need to offset for our draw more than half of our vblank, something is very wrong.
			// Clamp our max time to half of the vblank if we can.
			ulNewRollingDrawTime = std::min( ulNewRollingDrawTime, ulRefreshInterval - ulRedZone );

			// If this is not a pre-emptive re-arming, then update
			// the rolling internal max draw time for next time.
			if ( !bPreemptive )
				m_ulRollingMaxDrawTime = ulNewRollingDrawTime;

			ulOffset = ulNewRollingDrawTime + ulRedZone;

			if ( vblank_debug && !bPreemptive )
				VBlankDebugSpew( ulOffset, ulDrawTime, ulRedZone );
		}
		else
		{
			// See above.
			if ( !bPreemptive )
			{
				// Reset the max draw time to default, it is unused for VRR.
				m_ulRollingMaxDrawTime = kStartingVBlankDrawTime;
			}

			uint64_t ulRedZone = kVRRFlushingTime;

			uint64_t ulDrawTime = 0;
			/// See comment of m_ulVBlankDrawTimeMinCompositing.
			if ( m_bCurrentlyCompositing )
				ulDrawTime = std::max( ulDrawTime, m_ulVBlankDrawTimeMinCompositing );

			ulOffset = ulDrawTime + ulRedZone;
			eKind = LeadKind::Vrr;

			if ( vblank_debug && !bPreemptive )
				VBlankDebugSpew( ulOffset, ulDrawTime, ulRedZone );
		}

		m_ulLastLead.store( ulOffset, std::memory_order_relaxed );
		m_eLastLeadKind.store( eKind, std::memory_order_relaxed );

		uint64_t ulScheduledWakeupPoint = GetNextVBlank( ulOffset );
		uint64_t ulTargetVBlank = ulScheduledWakeupPoint + ulOffset;

		// The wake-up only has to lie in the future, so a lead that shrank since the last
		// wake-up (or a grid a feedback moved) can aim at the vblank that wake-up already
		// served: a second tick for one refresh. Counted always; ritz_timer_one_per_vblank
		// moves it to the next refresh.
		if ( ulRefreshInterval && m_ulLastFiredTarget && ulTargetVBlank < m_ulLastFiredTarget + ulRefreshInterval / 2 )
		{
			m_Stats.ulRetarget.fetch_add( 1, std::memory_order_relaxed );
			if ( cv_ritz_timer_one_per_vblank )
			{
				const uint64_t ulSteps = ( m_ulLastFiredTarget + ulRefreshInterval / 2 - ulTargetVBlank ) / ulRefreshInterval + 1;
				ulScheduledWakeupPoint += ulSteps * ulRefreshInterval;
				ulTargetVBlank += ulSteps * ulRefreshInterval;
			}
		}

		VBlankScheduleTime schedule =
		{
			.ulTargetVBlank = ulTargetVBlank,
			.ulScheduledWakeupPoint = ulScheduledWakeupPoint,
		};
		return schedule;
	}

	std::optional<VBlankTime> CVBlankTimer::ProcessVBlank()
	{
		return std::exchange( m_PendingVBlank, std::nullopt );
	}

	// Field test (2026-11-23): the refresh timer ticking faster than the refresh on a
	// nested Wayland host. Defaults = the behaviour before these switches.
	ConVar<bool> cv_ritz_timer_draw_per_tick( "ritz_timer_draw_per_tick", false,
		"Field test: 1 = only the first paint after a timer wake-up (vblank or frame generation's extra timer) feeds the "
		"wake-up lead's draw time; other paints (async, forced, arrival) record nothing. 0 = every paint records one (before)." );
	ConVar<int> cv_ritz_timer_feedback_anchor( "ritz_timer_feedback_anchor", 0,
		"Field test, nested Wayland: how a host `presented` feedback steers the vblank timer. 0 = every one anchors the "
		"grid and re-arms the timer at once (before). 1 = only a plausible vblank anchors and re-arms (at least half a "
		"refresh after the anchor and within ritz_timer_grid_tolerance of its grid; every 30th rejected one in a row is taken anyway to follow "
		"a real phase change). 2 = the same filter, but the anchor is never forced: the next tick's own re-arm picks it "
		"up. 3 = as 0, but feedbacks of commits that attached no new buffer are ignored. 4 = host feedback never touches "
		"the timer: it free-runs on its last grid at the refresh period (a control; drifts against the host's real vblank)." );
	ConVar<int> cv_ritz_timer_grid_tolerance( "ritz_timer_grid_tolerance", 5,
		"Field test: how far (percent of a refresh, 1-50) a host `presented` time may sit from the timer's grid and still "
		"count as on it: the `status: timer` off-grid count and ritz_timer_feedback_anchor 1/2 use it." );
	ConVar<bool> cv_ritz_timer_one_per_vblank( "ritz_timer_one_per_vblank", false,
		"Field test: 1 = a timer schedule never aims at the vblank the last wake-up already served (a lead that shrank, or "
		"a grid a feedback moved earlier, would otherwise give that refresh a second tick); it takes the next one. 0 = before." );

	void CVBlankTimer::MarkVBlank( uint64_t ulNanos, bool bReArmTimer, VBlankSource eSource, bool bNoNewBuffer )
	{
		m_Stats.ulMark[ size_t( eSource ) & 3 ].fetch_add( 1, std::memory_order_relaxed );

		if ( eSource == VBlankSource::HostFeedback )
		{
			const int nRefresh = GetRefresh();
			const uint64_t ulPeriod = nRefresh > 0 ? mHzToRefreshCycle( nRefresh ) : 0;
			const uint64_t ulPrevFb = m_ulLastHostFeedback.exchange( ulNanos, std::memory_order_relaxed );
			if ( ulPeriod && ulPrevFb && ( ulNanos <= ulPrevFb || ulNanos - ulPrevFb < ulPeriod / 2 ) )
				m_Stats.ulFbClose.fetch_add( 1, std::memory_order_relaxed );

			// Distance from the grid the current anchor defines (signed: + = later than
			// the nearest grid point).
			const uint64_t ulAnchor = m_ulLastVBlank.load();
			bool bOnGrid = true;
			if ( ulPeriod && ulAnchor )
			{
				const int64_t lDiff = int64_t( ulNanos - ulAnchor );
				const int64_t lPeriod = int64_t( ulPeriod );
				const int64_t lPhase = ( ( lDiff % lPeriod ) + lPeriod ) % lPeriod;
				const int64_t lSigned = lPhase > lPeriod / 2 ? lPhase - lPeriod : lPhase;
				const int nTol = std::clamp( int( cv_ritz_timer_grid_tolerance ), 1, 50 );
				bOnGrid = std::abs( lSigned ) * 100 <= lPeriod * nTol;
				m_Stats.ulPhaseN.fetch_add( 1, std::memory_order_relaxed );
				m_Stats.lPhaseSum.fetch_add( lSigned, std::memory_order_relaxed );
				m_Stats.ulPhaseAbsSum.fetch_add( uint64_t( std::abs( lSigned ) ), std::memory_order_relaxed );
			}
			if ( !bOnGrid )
				m_Stats.ulFbOffGrid.fetch_add( 1, std::memory_order_relaxed );

			const int nMode = cv_ritz_timer_feedback_anchor;
			bool bTake = true;
			if ( nMode == 4 )
				bTake = false;   // free-run: the timer keeps the grid it has
			else if ( nMode == 3 )
				bTake = !bNoNewBuffer;
			else if ( ( nMode == 1 || nMode == 2 ) && ulPeriod && ulAnchor )
			{
				const bool bPlausible = bOnGrid && ulNanos > ulAnchor && ulNanos - ulAnchor >= ulPeriod / 2;
				if ( bPlausible )
					m_nFbRejectRun.store( 0, std::memory_order_relaxed );
				else if ( m_nFbRejectRun.fetch_add( 1, std::memory_order_relaxed ) + 1 >= 30 )
					m_nFbRejectRun.store( 0, std::memory_order_relaxed );   // a real phase change: follow it
				else
					bTake = false;
			}
			if ( !bTake )
			{
				m_Stats.ulFbIgnored.fetch_add( 1, std::memory_order_relaxed );
				return;
			}

			m_ulLastVBlank = ulNanos;
			if ( bReArmTimer && nMode != 2 )
			{
				m_Stats.ulFbRearm.fetch_add( 1, std::memory_order_relaxed );
				ArmNextVBlank( false );
			}
			return;
		}

		m_ulLastVBlank = ulNanos;
		if ( bReArmTimer )
		{
			// Force timer re-arm with the new vblank timings.
			ArmNextVBlank( false );
		}
	}

	template <typename T>
	static void AtomicMax( std::atomic<T> &a, T v )
	{
		T cur = a.load( std::memory_order_relaxed );
		while ( v > cur && !a.compare_exchange_weak( cur, v, std::memory_order_relaxed ) ) {}
	}
	template <typename T>
	static void AtomicMin( std::atomic<T> &a, T v )
	{
		T cur = a.load( std::memory_order_relaxed );
		while ( v < cur && !a.compare_exchange_weak( cur, v, std::memory_order_relaxed ) ) {}
	}

	void CVBlankTimer::ReportDrawTime( uint64_t ulNanos )
	{
		const bool bFromWake = m_bDrawSamplePending.exchange( false, std::memory_order_relaxed );
		m_Stats.ulDrawSamples.fetch_add( 1, std::memory_order_relaxed );
		AtomicMax( m_Stats.ulMaxDraw, ulNanos );
		if ( !bFromWake )
		{
			m_Stats.ulDrawNonWake.fetch_add( 1, std::memory_order_relaxed );
			AtomicMax( m_Stats.ulMaxDrawNonWake, ulNanos );
			if ( cv_ritz_timer_draw_per_tick )
			{
				m_Stats.ulDrawSkipped.fetch_add( 1, std::memory_order_relaxed );
				return;
			}
		}
		UpdateLastDrawTime( ulNanos );
	}

	void CVBlankTimer::NoteHostCommit( bool bNoNewBuffer )
	{
		m_Stats.ulHostCommits.fetch_add( 1, std::memory_order_relaxed );
		if ( bNoNewBuffer )
			m_Stats.ulHostSameBuffer.fetch_add( 1, std::memory_order_relaxed );
	}

	void CVBlankTimer::NoteHostLatency( int64_t lLatency, bool bNoNewBuffer )
	{
		const int nRefresh = GetRefresh();
		const int64_t lPeriod = nRefresh > 0 ? int64_t( mHzToRefreshCycle( nRefresh ) ) : 0;
		m_Stats.ulLatN.fetch_add( 1, std::memory_order_relaxed );
		m_Stats.lLatSum.fetch_add( lLatency, std::memory_order_relaxed );
		AtomicMin( m_Stats.lLatMin, lLatency );
		AtomicMax( m_Stats.lLatMax, lLatency );
		if ( lPeriod && lLatency < lPeriod / 4 )
			m_Stats.ulLatEarly.fetch_add( 1, std::memory_order_relaxed );
		if ( bNoNewBuffer )
			m_Stats.ulFbSameBuffer.fetch_add( 1, std::memory_order_relaxed );
	}

	VBlankTimerStats CVBlankTimer::TakeStats()
	{
		auto X = []( auto &a, auto v ) { return a.exchange( v, std::memory_order_relaxed ); };
		Counters_t &C = m_Stats;
		VBlankTimerStats S;
		S.ulDrawSamples = X( C.ulDrawSamples, 0ul );
		S.ulDrawNonWake = X( C.ulDrawNonWake, 0ul );
		S.ulDrawSkipped = X( C.ulDrawSkipped, 0ul );
		S.ulMaxDraw = X( C.ulMaxDraw, 0ul );
		S.ulMaxDrawNonWake = X( C.ulMaxDrawNonWake, 0ul );
		S.ulWakeups = X( C.ulWakeups, 0ul );
		S.ulShortGaps = X( C.ulShortGaps, 0ul );
		S.ulMinGap = X( C.ulMinGap, UINT64_MAX );
		if ( S.ulMinGap == UINT64_MAX )
			S.ulMinGap = 0;
		for ( size_t i = 0; i < 4; i++ )
			S.ulMark[ i ] = X( C.ulMark[ i ], 0ul );
		S.ulFbClose = X( C.ulFbClose, 0ul );
		S.ulFbOffGrid = X( C.ulFbOffGrid, 0ul );
		S.ulRearmN = X( C.ulRearmN, 0ul );
		S.ulRearmOverdue = X( C.ulRearmOverdue, 0ul );
		S.ulRetarget = X( C.ulRetarget, 0ul );
		S.lRearmShiftSum = X( C.lRearmShiftSum, int64_t( 0 ) );
		S.ulPhaseN = X( C.ulPhaseN, 0ul );
		S.ulPhaseAbsSum = X( C.ulPhaseAbsSum, 0ul );
		S.lPhaseSum = X( C.lPhaseSum, int64_t( 0 ) );
		S.ulFbRearm = X( C.ulFbRearm, 0ul );
		S.ulFbIgnored = X( C.ulFbIgnored, 0ul );
		S.ulRearmEarlier = X( C.ulRearmEarlier, 0ul );
		S.ulRearmLater = X( C.ulRearmLater, 0ul );
		S.ulHostCommits = X( C.ulHostCommits, 0ul );
		S.ulHostSameBuffer = X( C.ulHostSameBuffer, 0ul );
		S.ulLatN = X( C.ulLatN, 0ul );
		S.ulLatEarly = X( C.ulLatEarly, 0ul );
		S.ulFbSameBuffer = X( C.ulFbSameBuffer, 0ul );
		S.lLatSum = X( C.lLatSum, int64_t( 0 ) );
		S.lLatMin = X( C.lLatMin, INT64_MAX );
		S.lLatMax = X( C.lLatMax, INT64_MIN );
		if ( !S.ulLatN )
			S.lLatMin = S.lLatMax = 0;
		return S;
	}

	bool CVBlankTimer::WasCompositing() const
	{
		return m_bCurrentlyCompositing;
	}

	void CVBlankTimer::UpdateWasCompositing( bool bCompositing )
	{
		m_bCurrentlyCompositing = bCompositing;
	}

	void CVBlankTimer::UpdateLastDrawTime( uint64_t ulNanos )
	{
		m_ulLastDrawTime = ulNanos;

		std::unique_lock lock( m_DrawRingMutex );
		m_DrawRing[ m_nDrawRingNext ] = ulNanos;
		m_nDrawRingNext = ( m_nDrawRingNext + 1 ) % kDrawTimeWindow;
		if ( m_nDrawRingCount < kDrawTimeWindow )
			m_nDrawRingCount++;
		uint64_t ulMax = 0;
		for ( size_t i = 0; i < m_nDrawRingCount; i++ )
			ulMax = std::max( ulMax, m_DrawRing[ i ] );
		m_ulRecentMaxDrawTime.store( ulMax, std::memory_order_relaxed );
	}

	bool CVBlankTimer::UsesMeasuredLead() const
	{
		return vblank_measured_lead;
	}

	CVBlankTimer::LeadKind CVBlankTimer::GetLastLeadKind() const
	{
		return m_eLastLeadKind.load( std::memory_order_relaxed );
	}

	uint64_t CVBlankTimer::GetLastLead() const
	{
		return m_ulLastLead.load( std::memory_order_relaxed );
	}

	uint64_t CVBlankTimer::GetRecentMaxDrawTime() const
	{
		return m_ulRecentMaxDrawTime.load( std::memory_order_relaxed );
	}

	uint64_t CVBlankTimer::GetRecentMeanDrawTime()
	{
		std::unique_lock lock( m_DrawRingMutex );
		if ( !m_nDrawRingCount )
			return 0;
		uint64_t ulSum = 0;
		for ( size_t i = 0; i < m_nDrawRingCount; i++ )
			ulSum += m_DrawRing[ i ];
		return ulSum / m_nDrawRingCount;
	}

	size_t CVBlankTimer::GetRecentDrawsOver( uint64_t ulLimit, size_t *pnCount )
	{
		std::unique_lock lock( m_DrawRingMutex );
		size_t nOver = 0;
		for ( size_t i = 0; i < m_nDrawRingCount; i++ )
			nOver += m_DrawRing[ i ] > ulLimit ? 1 : 0;
		*pnCount = m_nDrawRingCount;
		return nOver;
	}

	void CVBlankTimer::WaitToBeArmed()
	{
		// Wait for m_bArmed to change *from* false.
		m_bArmed.wait( false );
	}

	void CVBlankTimer::ArmNextVBlank( bool bPreemptive )
	{
		std::unique_lock lock( m_ScheduleMutex );

		// If we're pre-emptively re-arming, don't
		// do anything if we are already armed.
		if ( bPreemptive && m_bArmed )
			return;

		const bool bWasArmed = m_bArmed;
		m_bArmed = true;
		m_bArmed.notify_all();

		if ( UsingTimerFD() )
		{
			const uint64_t ulOldWakeup = m_TimerFDSchedule.ulScheduledWakeupPoint;
			m_TimerFDSchedule = CalcNextWakeupTime( bPreemptive );

			// `status: timer`: how far a forced re-arm moved a wake-up already armed.
			if ( !bPreemptive && bWasArmed )
			{
				const int nRefresh = GetRefresh();
				const int64_t lTenth = nRefresh > 0 ? int64_t( mHzToRefreshCycle( nRefresh ) / 100 ) : 0;   // 1%
				const int64_t lShift = int64_t( m_TimerFDSchedule.ulScheduledWakeupPoint - ulOldWakeup );
				m_Stats.ulRearmN.fetch_add( 1, std::memory_order_relaxed );
				// The armed wake-up was already due (its timerfd expiry not yet read): re-arming
				// replaces it, so that tick is lost or postponed.
				if ( ulOldWakeup && ulOldWakeup <= get_time_in_nanos() )
					m_Stats.ulRearmOverdue.fetch_add( 1, std::memory_order_relaxed );
				m_Stats.lRearmShiftSum.fetch_add( lShift, std::memory_order_relaxed );
				if ( lTenth && lShift < -lTenth )
					m_Stats.ulRearmEarlier.fetch_add( 1, std::memory_order_relaxed );
				else if ( lTenth && lShift > lTenth )
					m_Stats.ulRearmLater.fetch_add( 1, std::memory_order_relaxed );
			}

			ITimerWaitable::ArmTimer( m_TimerFDSchedule.ulScheduledWakeupPoint );
		}
	}

	bool CVBlankTimer::UsingTimerFD() const
	{
		return m_nNudgePipe[ 0 ] < 0;
	}

	int CVBlankTimer::GetFD()
	{
		return UsingTimerFD() ? ITimerWaitable::GetFD() : m_nNudgePipe[ 0 ];
	}

	void CVBlankTimer::OnPollIn()
	{
		if ( UsingTimerFD() )
		{
			std::unique_lock lock( m_ScheduleMutex );

			// Disarm the timer if it was armed.
			if ( !m_bArmed.exchange( false ) )
				return;


			m_PendingVBlank = VBlankTime
			{
				.schedule = m_TimerFDSchedule,
				// One might think this should just be 'now', however consider the fact
				// that the effective draw-time should also include the scheduling quantums
				// and any work before we reached this poll.
				// The old path used to be be on its own thread, simply awaking from sleep
				// then writing to a pipe and going back to sleep, the wakeup time was before we
				// did the write, so we included the quantum of pipe nudge -> wakeup.
				// Doing this aims to include that, like we were before, but with timerfd.
				.ulWakeupTime = m_TimerFDSchedule.ulScheduledWakeupPoint,
			};

			// `status: timer`: the gap from the previous wake-up.
			{
				const uint64_t ulWake = m_TimerFDSchedule.ulScheduledWakeupPoint;
				m_Stats.ulWakeups.fetch_add( 1, std::memory_order_relaxed );
				if ( m_ulLastWakeupPoint && ulWake > m_ulLastWakeupPoint )
				{
					const uint64_t ulGap = ulWake - m_ulLastWakeupPoint;
					AtomicMin( m_Stats.ulMinGap, ulGap );
					const int nRefresh = GetRefresh();
					if ( nRefresh > 0 && ulGap * 10 < mHzToRefreshCycle( nRefresh ) * 9 )
						m_Stats.ulShortGaps.fetch_add( 1, std::memory_order_relaxed );
				}
				m_ulLastWakeupPoint = ulWake;
				m_ulLastFiredTarget = m_TimerFDSchedule.ulTargetVBlank;
			}

			gpuvis_trace_printf( "vblank timerfd wakeup" );

			ITimerWaitable::DisarmTimer();
		}
		else
		{
			VBlankTime time{};
			for ( ;; )
			{
				ssize_t ret = read( m_nNudgePipe[ 0 ], &time, sizeof( time ) );

				if ( ret < 0 )
				{
					if ( errno == EAGAIN )
						continue;

					g_VBlankLog.errorf_errno( "Failed to read nudge pipe. Pre-emptively re-arming." );
					ArmNextVBlank( true );
					return;
				}
				else if ( ret != sizeof( VBlankTime ) )
				{
					g_VBlankLog.errorf( "Nudge pipe had less data than sizeof( VBlankTime ). Pre-emptively re-arming." );
					ArmNextVBlank( true );
					return;
				}
				else
				{
					break;
				}
			}

			uint64_t ulDiff = get_time_in_nanos() - time.ulWakeupTime;
			if ( ulDiff > 1'000'000ul )
			{
				gpuvis_trace_printf( "Ignoring stale vblank... Pre-emptively re-arming." );
				ArmNextVBlank( true );
				return;
			}

			gpuvis_trace_printf( "got vblank" );
			m_PendingVBlank = time;
		}
	}

	void CVBlankTimer::VBlankDebugSpew( uint64_t ulOffset, uint64_t ulDrawTime, uint64_t ulRedZone )
	{
		static uint64_t s_ulVBlankID = 0;
		static uint64_t s_ulLastDrawTime = kStartingVBlankDrawTime;
		static uint64_t s_ulLastOffset = kStartingVBlankDrawTime + ulRedZone;

		if ( s_ulVBlankID++ % 300 == 0 || ulDrawTime > s_ulLastOffset )
		{
			if ( ulDrawTime > s_ulLastOffset )
				g_VBlankLog.infof( " !! missed vblank " );

			g_VBlankLog.infof( "redZone: %.2fms decayRate: %lu%% - rollingMaxDrawTime: %.2fms lastDrawTime: %.2fms lastOffset: %.2fms - drawTime: %.2fms offset: %.2fms",
				ulRedZone / 1'000'000.0,
				m_ulVBlankRateOfDecayPercentage,
				m_ulRollingMaxDrawTime / 1'000'000.0,
				s_ulLastDrawTime / 1'000'000.0,
				s_ulLastOffset / 1'000'000.0,
				ulDrawTime / 1'000'000.0,
				ulOffset / 1'000'000.0 );
		}

		s_ulLastDrawTime = ulDrawTime;
		s_ulLastOffset = ulOffset;
	}

	void CVBlankTimer::NudgeThread()
	{
		pthread_setname_np( pthread_self(), "gamescope-vblk" );

		for ( ;; )
		{
			WaitToBeArmed();

			if ( !m_bRunning )
				return;

			VBlankScheduleTime schedule;
			if ( GetBackend()->GetCurrentConnector() )
			{
				schedule = GetBackend()->GetCurrentConnector()->FrameSync();
			}
			else
			{
				// If we don't currently have a connector, make up some dummy refresh cycle.
				sleep_for_nanos( mHzToRefreshCycle( g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh ) );
        		uint64_t ulNow = get_time_in_nanos();
				schedule = VBlankScheduleTime
				{
					.ulTargetVBlank  = ulNow + 3'000'000,
					.ulScheduledWakeupPoint = ulNow,
				};
			}

			const uint64_t ulWakeupTime = get_time_in_nanos();
			{
				std::unique_lock lock( m_ScheduleMutex );

				// Unarm, we are processing now!
				m_bArmed = false;

				VBlankTime timeInfo =
				{
					.schedule = schedule,
					.ulWakeupTime = ulWakeupTime,
				};

				ssize_t ret = write( m_nNudgePipe[ 1 ], &timeInfo, sizeof( timeInfo ) );
				if ( ret <= 0 )
				{
					g_VBlankLog.errorf_errno( "Nudge write failed" );
				}
				else
				{
					gpuvis_trace_printf( "sent vblank (nudge thread)" );
				}
			}
		}
	}
}

gamescope::CVBlankTimer &GetVBlankTimer()
{
    static gamescope::CVBlankTimer s_VBlankTimer;
    return s_VBlankTimer;
}

