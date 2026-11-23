#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include "waitable.h"
#include "convar.h"

namespace gamescope
{
    // display.low_latency_wakeup (Display > General) mirrors into this; default on.
    // On: every non-VRR wake-up lead is sized from the measured draw time; off:
    // upstream's rolling average + red zone heuristic, always.
    extern ConVar<bool> vblank_measured_lead;

    // Test switches of the refresh timer against host feedback (2026-11-23; the defaults
    // are the fix). See CVBlankTimer::ReportDrawTime() / MarkVBlank() / CalcNextWakeupTime().
    extern ConVar<bool> cv_ritz_timer_draw_per_tick;
    extern ConVar<int> cv_ritz_timer_feedback_anchor;
    extern ConVar<int> cv_ritz_timer_grid_tolerance;
    extern ConVar<bool> cv_ritz_timer_one_per_vblank;

    // The timer's scheduling decisions as pure functions (tests/test_vblank_schedule.cpp).
    // Times in ns. See superdoc/features/frame-generation-host-scanout.md, "The refresh
    // timer and the host's feedback".
    namespace vblanksched
    {
        // The first anchor + k * period - lead (k >= 1) that is not before now: the next
        // wake-up on the grid the anchor defines. Free-running, it advances in whole periods.
        constexpr uint64_t NextWakeup( uint64_t ulAnchor, uint64_t ulPeriod, uint64_t ulLead, uint64_t ulNow )
        {
            uint64_t ulWake = ulAnchor + ulPeriod - ulLead;
            if ( ulWake < ulNow && ulPeriod )
                ulWake += ( ( ulNow - ulWake + ulPeriod - 1 ) / ulPeriod ) * ulPeriod;
            return ulWake;
        }

        // One tick per vblank: whole periods to add to a schedule whose target vblank is
        // the one the last wake-up already served (less than half a period after it). The
        // wake-up only has to lie in the future, so a lead that shrank after a paint, or a
        // grid moved earlier, would otherwise give that refresh a second tick.
        constexpr uint64_t OnePerVBlankSteps( uint64_t ulTarget, uint64_t ulLastFiredTarget, uint64_t ulPeriod )
        {
            if ( !ulPeriod || !ulLastFiredTarget || ulTarget >= ulLastFiredTarget + ulPeriod / 2 )
                return 0;
            return ( ulLastFiredTarget + ulPeriod / 2 - ulTarget ) / ulPeriod + 1;
        }

        // Signed distance of a time from the nearest point of the grid an anchor defines
        // (+ = later), in (-period/2, period/2].
        constexpr int64_t GridPhase( uint64_t ulStamp, uint64_t ulAnchor, uint64_t ulPeriod )
        {
            if ( !ulPeriod )
                return 0;
            const int64_t lPeriod = int64_t( ulPeriod );
            const int64_t lDiff = int64_t( ulStamp - ulAnchor );
            const int64_t lPhase = ( ( lDiff % lPeriod ) + lPeriod ) % lPeriod;
            return lPhase > lPeriod / 2 ? lPhase - lPeriod : lPhase;
        }

        constexpr bool OnGrid( int64_t lPhase, uint64_t ulPeriod, int nTolerancePct )
        {
            const int64_t lAbs = lPhase < 0 ? -lPhase : lPhase;
            return lAbs * 100 <= int64_t( ulPeriod ) * nTolerancePct;
        }

        // Which host `presented` times may anchor the grid (ritz_timer_feedback_anchor 1/2).
        // A host showing a frame at once (VRR below its maximum, a host that tears) stamps
        // it shortly after gamescope's commit, early against the vblank gamescope aimed at;
        // anchoring on that moves the grid earlier each time and the timer runs fast.
        struct AnchorFilter
        {
            // Rejected stamps in a row that all lie on ONE grid (not ours) before that grid
            // is taken: a host that really moved its vblank phase (a restart, a mode change,
            // long drift). Stamps that follow gamescope's own commits do not line up.
            static constexpr uint32_t kAgree = 30;

            uint64_t ulCandidate = 0;   // first stamp of the current run of rejected ones
            uint32_t nRun = 0;

            // True: take ulStamp as the new anchor.
            bool Decide( uint64_t ulStamp, uint64_t ulAnchor, uint64_t ulPeriod, int nTolerancePct )
            {
                if ( !ulPeriod || !ulAnchor )
                    return true;
                if ( OnGrid( GridPhase( ulStamp, ulAnchor, ulPeriod ), ulPeriod, nTolerancePct ) )
                {
                    if ( ulStamp > ulAnchor && ulStamp - ulAnchor >= ulPeriod / 2 )
                    {
                        nRun = 0;
                        return true;
                    }
                    return false;   // the anchor's own vblank again, or older: nothing to learn
                }
                if ( nRun && ulStamp > ulCandidate &&
                     OnGrid( GridPhase( ulStamp, ulCandidate, ulPeriod ), ulPeriod, nTolerancePct ) )
                {
                    if ( ++nRun >= kAgree )
                    {
                        nRun = 0;
                        return true;
                    }
                }
                else
                {
                    ulCandidate = ulStamp;
                    nRun = 1;
                }
                return false;
            }
        };

        // The host's refresh is not the grid's (a nested refresh below the output's): its
        // vblanks are no points of the timer's grid at all. 0.5% apart or more.
        constexpr bool ForeignGrid( int nGridmHz, int nHostmHz )
        {
            if ( nGridmHz <= 0 || nHostmHz <= 0 )
                return false;
            const int64_t lDiff = int64_t( nGridmHz ) - int64_t( nHostmHz );
            return ( lDiff < 0 ? -lDiff : lDiff ) * 200 > int64_t( nHostmHz );
        }
    }

    // Who reported a vblank to MarkVBlank(), for the `status: timer` line. Other is
    // the present-wait thread of the Vulkan swapchain output (rendervulkan.cpp).
    enum class VBlankSource { Other, HostFeedback, Drm, OpenVR };

    // The `status: timer` line's counters since the previous TakeStats(), ns for times.
    struct VBlankTimerStats
    {
        uint64_t ulDrawSamples = 0;      // draw times reported by a backend's Present()
        uint64_t ulDrawNonWake = 0;      // ... of a paint that was not the first after a wake-up
        uint64_t ulDrawSkipped = 0;      // ... of those, dropped (ritz_timer_draw_per_tick 1)
        uint64_t ulMaxDraw = 0;          // largest draw time reported
        uint64_t ulMaxDrawNonWake = 0;   // largest non-wake-up draw time reported
        uint64_t ulWakeups = 0;          // timerfd wake-ups
        uint64_t ulShortGaps = 0;        // wake-ups less than 0.9 refresh after the previous one
        uint64_t ulMinGap = 0;           // smallest gap between two wake-ups (0 = fewer than two)
        uint64_t ulMark[ 4 ] = {};       // MarkVBlank() calls by VBlankSource
        uint64_t ulFbClose = 0;          // host feedbacks < 0.5 refresh after the previous one
        uint64_t ulFbOffGrid = 0;        // host feedbacks off the anchor's grid by more than ritz_timer_grid_tolerance
        uint64_t ulPhaseN = 0;           // host feedbacks measured against the grid
        int64_t  lPhaseSum = 0;          // ... their signed distance from it (+ = late), summed
        uint64_t ulPhaseAbsSum = 0;      // ... and their absolute distance, summed
        uint64_t ulFbRearm = 0;          // forced re-arms by a host feedback
        uint64_t ulFbIgnored = 0;        // host feedbacks not taken as the anchor (switch)
        uint64_t ulRearmEarlier = 0;     // forced re-arms that moved an armed wake-up earlier (> 1% of a refresh)
        uint64_t ulRearmLater = 0;       // ... later (> 1%)
        uint64_t ulRearmN = 0;           // forced re-arms of an armed timer (any source)
        uint64_t ulRearmOverdue = 0;     // ... whose replaced wake-up was already due (a lost tick)
        uint64_t ulRetarget = 0;         // schedules aimed at the vblank the last wake-up already served
        uint64_t ulRearmKept = 0;        // forced re-arms skipped because the armed wake-up was due
        uint64_t ulTargetShort = 0;      // wake-ups aimed less than 0.9 refresh after the previous one's vblank
        uint64_t ulMissed = 0;           // refreshes between two wake-ups' vblanks that no wake-up aimed at
        int64_t  lRearmShiftSum = 0;     // ... how far they moved its wake-up, summed (+ = later)
        uint64_t ulHostCommits = 0;      // commits to the host that asked for feedback
        uint64_t ulHostSameBuffer = 0;   // ... of those, without a new buffer (no attach)
        uint64_t ulLatN = 0;             // presented feedbacks with a known commit time
        int64_t  lLatSum = 0, lLatMin = 0, lLatMax = 0;   // presented time - commit time
        uint64_t ulLatEarly = 0;         // latency under a quarter refresh
        uint64_t ulFbSameBuffer = 0;     // presented feedbacks of a commit without a new buffer
    };

    struct VBlankScheduleTime
    {
        // The expected time for the vblank we want to target.
        uint64_t ulTargetVBlank = 0;
        // The vblank offset by the redzone/scheduling calculation.
        // This is when we want to wake-up by to meet that vblank time above.
        uint64_t ulScheduledWakeupPoint = 0;
    };

    struct VBlankTime
    {
        VBlankScheduleTime schedule;
        // This is when we woke-up either by the timerfd poll
        // or on the nudge thread. We use this to feed-back into
        // the draw time so we automatically account for th
        //  CPU scheduler quantums.
        uint64_t ulWakeupTime = 0;
    };

    class CVBlankTimer : public ITimerWaitable
    {
    public:
        static constexpr uint64_t kMilliSecInNanoSecs = 1'000'000ul;
        // VBlank timer defaults and starting values.
        // Anything time-related is nanoseconds unless otherwise specified.
        static constexpr uint64_t kStartingVBlankDrawTime = 3'000'000ul;
        static constexpr uint64_t kDefaultMinVBlankTime = 350'000ul;
        static constexpr uint64_t kDefaultVBlankRedZone = 1'650'000ul;
        static constexpr uint64_t kDefaultVBlankDrawTimeMinCompositing = 2'400'000ul;
        static constexpr uint64_t kDefaultVBlankRateOfDecayPercentage = 980ul; // 98%
        static constexpr uint64_t kVBlankRateOfDecayMax = 1000ul; // 100%

        static constexpr uint64_t kVRRFlushingTime = 300'000;

        // Measured wake-up lead (vblank_measured_lead, the "Low-latency wake-up"
        // setting, on by default): the lead is the largest of the last kDrawTimeWindow
        // draws plus this margin, instead of the rolling average + red zone (+ the
        // 2.4 ms compositing floor) that gives about 4 ms whatever the draw costs.
        static constexpr size_t   kDrawTimeWindow = 60;
        static constexpr uint64_t kMeasuredLeadMargin = 750'000ul;

        // The measured lead, ns, for a recent-max draw time (0 = none measured yet)
        // and a refresh interval. The draw is floored at the margin itself, so the
        // lead never goes under 2 x kMeasuredLeadMargin (1.5 ms): a direct-scanout
        // frame measures tiny draws (a flip is not a composite), and a lead sized
        // from those would miss the first refresh after an overlay or a composite
        // appears. Capped at one refresh: past that the wake-up just slips a vblank.
        static constexpr uint64_t MeasuredLead( uint64_t ulRecentMaxDraw, uint64_t ulRefreshInterval )
        {
            const uint64_t ulDraw = ulRecentMaxDraw ? ulRecentMaxDraw : kStartingVBlankDrawTime;
            const uint64_t ulFloored = ulDraw > kMeasuredLeadMargin ? ulDraw : kMeasuredLeadMargin;
            const uint64_t ulLead = ulFloored + kMeasuredLeadMargin;
            return ulLead < ulRefreshInterval ? ulLead : ulRefreshInterval;
        }

        CVBlankTimer();
        ~CVBlankTimer();

        int GetRefresh() const;
        uint64_t GetLastVBlank() const;
        uint64_t GetNextVBlank( uint64_t ulOffset ) const;

        VBlankScheduleTime CalcNextWakeupTime( bool bPreemptive );
        void Reschedule();

        std::optional<VBlankTime> ProcessVBlank();
        // HostFeedback goes through ritz_timer_feedback_anchor's filter; every other source
        // anchors and re-arms as upstream does.
        void MarkVBlank( uint64_t ulNanos, bool bReArmTimer, VBlankSource eSource = VBlankSource::Other );

        // Draw-time samples (ritz_timer_draw_per_tick): steamcompmgr arms one at a
        // timer (or frame generation's extra timer) wake-up and disarms it at the end
        // of that loop iteration; a backend's Present() reports through ReportDrawTime().
        void ArmDrawSample() { m_bDrawSamplePending.store( true, std::memory_order_relaxed ); }
        void DisarmDrawSample() { m_bDrawSamplePending.store( false, std::memory_order_relaxed ); }
        void ReportDrawTime( uint64_t ulNanos );

        // Wayland backend: a commit that asked for feedback, and a presented feedback's
        // latency from that commit (ns; may be negative on a skewed clock).
        void NoteHostCommit( bool bNoNewBuffer );
        void NoteHostLatency( int64_t lLatency, bool bNoNewBuffer );
        VBlankTimerStats TakeStats();

        bool WasCompositing() const;
        void UpdateWasCompositing( bool bCompositing );
        void UpdateLastDrawTime( uint64_t ulNanos );

        // Whether the wake-up lead is sized from the measured draw time (the
        // vblank_measured_lead ConVar); off = byte-identical to upstream. (Reads
        // true under VRR too, which keeps upstream's fixed lead either way.)
        bool UsesMeasuredLead() const;
        // What the last schedule actually used, for the log: measured (non-VRR only),
        // VRR's fixed upstream lead, or upstream's fixed-ish non-VRR lead.
        enum class LeadKind { Default, Vrr, Measured };
        LeadKind GetLastLeadKind() const;
        // The lead the last (non-VRR or VRR) schedule used, ns: how long before its
        // vblank the timer wakes. For the log; 0 before the first schedule.
        uint64_t GetLastLead() const;
        // The largest of the last kDrawTimeWindow draw times, ns (0 = none yet).
        uint64_t GetRecentMaxDrawTime() const;
        // The mean of the last kDrawTimeWindow draw times, ns (0 = none yet), and how
        // many of them ran longer than ulLimit (a paint that ended after its vblank
        // when ulLimit is the wake-up lead); *pnCount = how many draws are in the ring.
        uint64_t GetRecentMeanDrawTime();
        size_t GetRecentDrawsOver( uint64_t ulLimit, size_t *pnCount );

        void WaitToBeArmed();
        void ArmNextVBlank( bool bPreemptive );
        // A host feedback's forced re-arm; bKeepDue: leave an armed wake-up that is already due.
        void ArmNextVBlankFromFeedback( bool bKeepDue );

        bool UsingTimerFD() const;
        int GetFD() final;
        void OnPollIn() final;
    private:
        void VBlankDebugSpew( uint64_t ulOffset, uint64_t ulDrawTime, uint64_t ulRedZone );

        uint64_t m_ulTargetVBlank = 0;
        std::atomic<uint64_t> m_ulLastVBlank = { 0 };
        std::atomic<bool> m_bArmed = { false };
        std::atomic<bool> m_bRunning = { true };

        std::optional<VBlankTime> m_PendingVBlank;

        // Should have 0 contest, but just to be safe.
        // This also covers setting of m_bArmed, etc
        // so we keep in sequence.
        // m_bArmed is atomic so can still be .wait()'ed
        // on/read outside.
        // Does not cover m_ulLastVBlank, this is just atomic.
        std::mutex m_ScheduleMutex;
        VBlankScheduleTime m_TimerFDSchedule{};

        std::thread m_NudgeThread;
        int m_nNudgePipe[2] = { -1, -1 };

        /////////////////////////////
        // Scheduling bits and bobs.
        /////////////////////////////

        // Are we currently compositing? We may need
        // to push back to avoid clock feedback loops if so.
        // This is fed-back from steamcompmgr.
        std::atomic<bool> m_bCurrentlyCompositing = { false };
        // This is the last time a 'draw' took from wake-up to page flip.
        // 3ms by default to get the ball rolling.
        // This is calculated by steamcompmgr/drm and fed-back to the vblank timer.
        std::atomic<uint64_t> m_ulLastDrawTime = { kStartingVBlankDrawTime };

        // The last kDrawTimeWindow draw times, and their maximum (published for the
        // timer side). Written by whoever reports a draw (UpdateLastDrawTime()),
        // under its own mutex so a second reporting thread cannot tear the ring.
        std::mutex m_DrawRingMutex;
        std::array<uint64_t, kDrawTimeWindow> m_DrawRing{};
        size_t m_nDrawRingNext = 0;
        size_t m_nDrawRingCount = 0;
        std::atomic<uint64_t> m_ulRecentMaxDrawTime = { 0 };
        std::atomic<uint64_t> m_ulLastLead = { 0 };
        std::atomic<LeadKind> m_eLastLeadKind = { LeadKind::Default };

        //////////////////////////////////
        // VBlank timing tuneables below!
        //////////////////////////////////

        // Internal rolling peak exponential avg. draw time.
        // This is updated in CalcNextWakeupTime when not
        // doing pre-emptive timer re-arms.
        uint64_t m_ulRollingMaxDrawTime = kStartingVBlankDrawTime;

        // This accounts for some time we cannot account for (which (I think) is the drm_commit -> triggering the pageflip)
        // It would be nice to make this lower if we can find a way to track that effectively
        // Perhaps the missing time is spent elsewhere, but given we track from the pipe write
        // to after the return from `drm_commit` -- I am very doubtful.
        // 1.3ms by default. (kDefaultMinVBlankTime)
        uint64_t m_ulMinVBlankTime = kDefaultMinVBlankTime;

        // The leeway we always apply to our buffer.
        // 0.3ms by default. (kDefaultVBlankRedZone)
        uint64_t m_ulVBlankDrawBufferRedZone = kDefaultVBlankRedZone;

        // The minimum drawtime to use when we are compositing.
        // Getting closer and closer to vblank when compositing means that we can get into
        // a feedback loop with our GPU clocks. Pick a sane minimum draw time.
        // 2.4ms by default. (kDefaultVBlankDrawTimeMinCompositing)
        uint64_t m_ulVBlankDrawTimeMinCompositing = kDefaultVBlankDrawTimeMinCompositing;

        // The rate of decay (as a percentage) of the rolling average -> current draw time
        // 930 = 93%.
        // 93% by default. (kDefaultVBlankRateOfDecayPercentage)
        uint64_t m_ulVBlankRateOfDecayPercentage = kDefaultVBlankRateOfDecayPercentage;

        void NudgeThread();

        // Field-test counters (`status: timer`), all relaxed atomics.
        std::atomic<bool> m_bDrawSamplePending = { false };
        std::atomic<uint64_t> m_ulLastHostFeedback = { 0 };
        std::mutex m_AnchorFilterMutex;
        vblanksched::AnchorFilter m_AnchorFilter;
        void ArmNextVBlankImpl( bool bPreemptive, bool bKeepDue );
        uint64_t m_ulLastWakeupPoint = 0;   // under m_ScheduleMutex
        uint64_t m_ulLastFiredTarget = 0;   // the vblank the last timerfd wake-up aimed at; under m_ScheduleMutex
        struct Counters_t
        {
            std::atomic<uint64_t> ulDrawSamples{ 0 }, ulDrawNonWake{ 0 }, ulDrawSkipped{ 0 }, ulMaxDraw{ 0 }, ulMaxDrawNonWake{ 0 };
            std::atomic<uint64_t> ulWakeups{ 0 }, ulShortGaps{ 0 }, ulMinGap{ UINT64_MAX };
            std::atomic<uint64_t> ulMark[ 4 ]{};
            std::atomic<uint64_t> ulFbClose{ 0 }, ulFbOffGrid{ 0 }, ulFbRearm{ 0 }, ulFbIgnored{ 0 }, ulRearmEarlier{ 0 }, ulRearmLater{ 0 };
            std::atomic<uint64_t> ulHostCommits{ 0 }, ulHostSameBuffer{ 0 }, ulLatN{ 0 }, ulLatEarly{ 0 }, ulFbSameBuffer{ 0 };
            std::atomic<int64_t> lLatSum{ 0 }, lLatMin{ INT64_MAX }, lLatMax{ INT64_MIN };
            std::atomic<uint64_t> ulPhaseN{ 0 }, ulPhaseAbsSum{ 0 };
            std::atomic<int64_t> lPhaseSum{ 0 }, lRearmShiftSum{ 0 };
            std::atomic<uint64_t> ulRearmN{ 0 }, ulRearmOverdue{ 0 }, ulRetarget{ 0 }, ulRearmKept{ 0 }, ulTargetShort{ 0 }, ulMissed{ 0 };
        } m_Stats;
    };
}

gamescope::CVBlankTimer &GetVBlankTimer();

