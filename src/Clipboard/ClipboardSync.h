// Clipboard sync: the compositor-free core.
//
// The two pieces of clipboard-sync logic that are worth getting exactly right
// are also the two that need neither a Wayland connection nor an X server, so
// they live here and are covered by tests/test_clipboard_sync.cpp.
//
// See superdoc/features/clipboard-sync.md for the whole feature.

#pragma once

#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

namespace gamescope
{
    // Hard ceiling on a single clipboard transfer. A host application is free
    // to hand us a pipe it never stops writing to; without a cap that is an
    // unbounded allocation driven by another process.
    static constexpr size_t k_nMaxClipboardBytes = 16 * 1024 * 1024;

    // Rank a mime type as a source of plain text. 0 means "not text".
    // Higher is better; UTF-8-explicit types beat ambiguous ones.
    inline int RankClipboardMime( const char *pMime )
    {
        if ( !pMime )
            return 0;
        if ( !strcmp( pMime, "text/plain;charset=utf-8" ) )
            return 4;
        if ( !strcmp( pMime, "UTF8_STRING" ) )
            return 3;
        if ( !strcmp( pMime, "text/plain" ) )
            return 2;
        if ( !strcmp( pMime, "STRING" ) || !strcmp( pMime, "TEXT" ) )
            return 1;
        return 0;
    }

    // The mime types we advertise when we own the host clipboard. Ordered
    // best-first, matching what the X11 selection handler already answers to
    // in steamcompmgr.cpp.
    inline constexpr const char *k_pszClipboardMimes[] =
    {
        "text/plain;charset=utf-8",
        "text/plain",
        "UTF8_STRING",
        "STRING",
        "TEXT",
    };

    // Tidy a blob that arrived from a clipboard pipe or an X11 selection
    // property into something we can hand on as text.
    //
    // - Trailing NULs are dropped. X11 STRING/UTF8_STRING owners often
    //   NUL-terminate; Wayland text/plain owners never do. Keeping the NUL
    //   would make a round-tripped string compare unequal to the original,
    //   which would defeat the loop guard below.
    // - Anything past k_nMaxClipboardBytes is dropped.
    //
    // Embedded NULs in the middle are left alone: they are legal in a
    // std::string, and truncating there would silently corrupt a paste.
    std::string NormalizeClipboardText( std::string sText );

    // The loop guard.
    //
    // Clipboard sync is a cycle by construction: the host tells us its
    // clipboard changed, we set the X11 selection, X notifies us that the
    // selection changed, and we tell the host. Without a brake that runs
    // forever.
    //
    // The X11 half of the cycle is already broken upstream by an identity
    // check (`event->owner == ctx->ourWindow`). The Wayland half has no such
    // check available: the data-control protocols hand us an offer with no way
    // to ask "is this the source I created?", so the only signal we have is
    // the content itself.
    //
    // That turns out to be enough, and is in fact the right test: a clipboard
    // set to text it already holds is a no-op no matter who set it, so
    // suppressing that transfer can never lose data. The guard remembers the
    // last value that crossed in each direction and refuses to send a value
    // straight back the way it came.
    // Drain a clipboard transfer pipe to EOF and return what it held.
    //
    // Takes ownership of nFd and always closes it. Returns nullopt if the
    // transfer failed or stalled -- a source that opens the pipe and then
    // never writes or closes must not hold the reader forever, so a quiet
    // period longer than nTimeoutMs aborts.
    //
    // This blocks, deliberately. It is only ever called on a short-lived
    // worker thread, never on the compositor thread. See the threading note
    // in superdoc/features/clipboard-sync.md.
    std::optional<std::string> ReadClipboardPipe( int nFd, int nTimeoutMs );

    // Push sText down a clipboard transfer pipe and close it.
    //
    // Blocks like ReadClipboardPipe, and for the same reason must only ever
    // run on a worker thread: the reader on the far end is an arbitrary
    // application, and a pipe whose 64 KiB buffer is full will not accept
    // another byte until that application reads. Takes ownership of nFd.
    void WriteClipboardPipe( int nFd, const std::string &sText, int nTimeoutMs );

    // How long a stalled transfer is given before it is abandoned.
    static constexpr int k_nClipboardTransferTimeoutMs = 2000;

    class CClipboardLoopGuard
    {
    public:
        // We are about to publish sText to the host compositor.
        // False means "this is what the host just told us" -- sending it back
        // would start the cycle.
        bool ShouldPushToHost( const std::string &sText );

        // The host compositor says its clipboard is now sText.
        // False means "this is our own push coming back to us".
        bool ShouldAcceptFromHost( const std::string &sText );

        // Forget both directions. Used when the host device goes away
        // (data-control `finished`), so a reconnect starts clean.
        void Reset();

    private:
        std::mutex m_Mutex;
        std::optional<std::string> m_oLastPushed;
        std::optional<std::string> m_oLastAccepted;
    };
}
