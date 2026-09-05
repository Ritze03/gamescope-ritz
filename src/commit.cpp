#include <atomic>

#include "wlserver.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "commit.h"

#include "gpuvis_trace_utils.h"

extern gamescope::CAsyncWaiter<gamescope::Rc<commit_t>> g_ImageWaiter;

// M4 FPS display (superdoc/planning/SPEC.md Feature 3, DECISIONS.md #16/#17):
// the game's own frametime, mirroring what feeds mangoapp's app_frametime_ns
// below -- but read from here instead of mangoapp's own mangoapp_msg_v1,
// because that struct is a single shared message-queue payload two
// independent call sites overwrite (this one, and mangoapp_output_update()'s
// visible-frametime path), so app_frametime_ns can transiently read back as
// mangoapp's own "not available" sentinel depending on write order. This
// global is written only here, so an in-process reader never sees that race.
// Atomic since Signal() can run off the steamcompmgr thread (g_ImageWaiter's
// own waiter thread, see AddWaitable() below) -- there is no other
// synchronization on this value.
std::atomic<uint64_t> g_ulLastAppFrametimeNs{ 0 };

// 2026-09-05: a running COUNT of focused-window commits, beside the last
// frametime above, for the HUD's frame-rate readout (FpsDisplay.cpp's
// UpdateAndGetDisplayFps() computes delta-count / delta-time over its own
// windows). Counting is the fix for the "999 FPS" bug: steamcompmgr drains
// every finished commit in one loop, and when two or more land in a batch
// (lsfg-vk presenting a real and a generated frame back to back guarantees
// this) the LAST Signal() in the batch sees a frametime of ~0 -- 10 000 fps
// once clamped -- and, reading at most one sample per paint, the HUD only
// ever saw that last write. A count cannot be fooled by batching: two
// commits are two commits whenever they arrived. Same atomic/relaxed
// reasoning as g_ulLastAppFrametimeNs.
std::atomic<uint64_t> g_ulAppCommitCount{ 0 };

commit_t::commit_t()
{
    static uint64_t maxCommmitID = 0;
    commitID = ++maxCommmitID;
}
commit_t::~commit_t()
{
    {
        std::unique_lock lock( m_WaitableCommitStateMutex );
        CloseFenceInternal();
    }

    if ( vulkanTex != nullptr )
        vulkanTex = nullptr;

    wlserver_lock();
    if (!presentation_feedbacks.empty())
    {
        wlserver_presentation_feedback_discard(surf, presentation_feedbacks);
        // presentation_feedbacks cleared by wlserver_presentation_feedback_discard
    }
    wlr_buffer_unlock( buf );
    wlserver_unlock();
}

GamescopeAppTextureColorspace commit_t::colorspace() const
{
    VkColorSpaceKHR colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (feedback && vulkanTex)
        colorspace = feedback->vk_colorspace;

    if (!vulkanTex)
        return GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;

    return VkColorSpaceToGamescopeAppTextureColorSpace(vulkanTex->format(), colorspace);
}

int commit_t::GetFD()
{
    return m_nCommitFence;
}

void commit_t::OnPollIn()
{
    gpuvis_trace_end_ctx_printf( commitID, "wait fence" );

    {
        std::unique_lock lock( m_WaitableCommitStateMutex );
        if ( !CloseFenceInternal() )
            return;
    }

    Signal();

    nudge_steamcompmgr();
}

void commit_t::Signal()
{
    uint64_t now = get_time_in_nanos();
    present_time = now;

    uint64_t frametime;
    if ( m_bMangoNudge )
    {
        static uint64_t lastFrameTime = now;
        frametime = now - lastFrameTime;
        lastFrameTime = now;
        g_ulLastAppFrametimeNs.store( frametime, std::memory_order_relaxed );
        // Batching (see g_ulAppCommitCount's comment) also fools the
        // frametime as a lag-spike signal: a batch of two writes a ~0 ms
        // sample, and the next real frame then measures ~2x the median.
        // Accepted for now -- the spike detector only reacts to that
        // pattern under frame generation, where the frametime is already
        // not the game's own -- and documented in
        // superdoc/features/fps-display.md.
        g_ulAppCommitCount.fetch_add( 1, std::memory_order_relaxed );
    }

    // TODO: Move this so it's called in the main loop.
    // Instead of looping over all the windows like before.
    // When we get the new IWaitable stuff in there.
    {
        std::unique_lock< std::mutex > lock( m_pDoneCommits->listCommitsDoneLock );
        m_pDoneCommits->listCommitsDone.push_back( CommitDoneEntry_t{
            .winSeq = win_seq,
            .commitID = commitID,
            .desiredPresentTime = desired_present_time,
            .fifo = fifo,
        } );
    }

    if ( m_bMangoNudge )
        mangoapp_update( uint64_t(~0ull), frametime, uint64_t(~0ull) );
}

void commit_t::OnPollHangUp()
{
    std::unique_lock lock( m_WaitableCommitStateMutex );
    CloseFenceInternal();
}

bool commit_t::IsPerfOverlayFIFO()
{
    return fifo || is_steam;
}

// Returns true if we had a fence that was closed.
bool commit_t::CloseFenceInternal()
{
    if ( m_nCommitFence < 0 )
        return false;

    // Will automatically remove from epoll!
    g_ImageWaiter.RemoveWaitable( this );
    close( m_nCommitFence );
    m_nCommitFence = -1;
    return true;
}

void commit_t::SetFence( int nFence, bool bMangoNudge, CommitDoneList_t *pDoneCommits )
{
    std::unique_lock lock( m_WaitableCommitStateMutex );
    CloseFenceInternal();

    m_nCommitFence = nFence;
    m_bMangoNudge = bMangoNudge;
    m_pDoneCommits = pDoneCommits;
}

void calc_scale_factor(float &out_scale_x, float &out_scale_y, float sourceWidth, float sourceHeight);

bool commit_t::ShouldPreemptivelyUpscale()
{
    // Don't pre-emptively upscale if we are not a FIFO commit.
    // Don't want to FSR upscale 1000fps content.
    if ( !fifo )
        return false;

    // If we support the upscaling filter in hardware, don't
    // pre-emptively do it via shaders.
    if ( DoesHardwareSupportUpscaleFilter( g_upscaleFilter ) )
        return false;

    if ( !vulkanTex )
        return false;

    float flScaleX = 1.0f;
    float flScaleY = 1.0f;
    // I wish this function was more programatic with its inputs, but it does do exactly what we want right now...
    // It should also return a std::pair or a glm uvec
    calc_scale_factor( flScaleX, flScaleY, vulkanTex->width(), vulkanTex->height() );

    return !close_enough( flScaleX, 1.0f ) || !close_enough( flScaleY, 1.0f );
}
