#include "Clipboard/ClipboardSync.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace gamescope
{
    std::string NormalizeClipboardText( std::string sText )
    {
        if ( sText.size() > k_nMaxClipboardBytes )
            sText.resize( k_nMaxClipboardBytes );

        size_t nEnd = sText.size();
        while ( nEnd > 0 && sText[ nEnd - 1 ] == '\0' )
            nEnd--;
        sText.resize( nEnd );

        return sText;
    }

    std::optional<std::string> ReadClipboardPipe( int nFd, int nTimeoutMs )
    {
        if ( nFd < 0 )
            return std::nullopt;

        std::string sOut;
        bool bOk = true;

        for ( ;; )
        {
            pollfd fd = { .fd = nFd, .events = POLLIN, .revents = 0 };

            int nRet = poll( &fd, 1, nTimeoutMs );
            if ( nRet < 0 )
            {
                if ( errno == EINTR )
                    continue;
                bOk = false;
                break;
            }
            if ( nRet == 0 )
            {
                // The source went quiet without closing. Give up rather than
                // leak this thread for the lifetime of the process.
                bOk = false;
                break;
            }

            char szBuf[ 4096 ];
            ssize_t nRead = read( nFd, szBuf, sizeof( szBuf ) );
            if ( nRead < 0 )
            {
                if ( errno == EINTR || errno == EAGAIN )
                    continue;
                bOk = false;
                break;
            }
            if ( nRead == 0 )
                break; // EOF: the transfer is complete.

            sOut.append( szBuf, size_t( nRead ) );

            if ( sOut.size() > k_nMaxClipboardBytes )
            {
                // Truncate rather than fail: a huge paste is still better than
                // no paste, and the cap is generous.
                sOut.resize( k_nMaxClipboardBytes );
                break;
            }
        }

        close( nFd );

        if ( !bOk && sOut.empty() )
            return std::nullopt;

        return NormalizeClipboardText( std::move( sOut ) );
    }

    void WriteClipboardPipe( int nFd, const std::string &sText, int nTimeoutMs )
    {
        if ( nFd < 0 )
            return;

        // A reader that goes away mid-transfer raises SIGPIPE on this thread's
        // process. gamescope already ignores SIGPIPE process-wide, so the
        // write just returns EPIPE, which ends the loop.
        size_t nOffset = 0;
        while ( nOffset < sText.size() )
        {
            pollfd fd = { .fd = nFd, .events = POLLOUT, .revents = 0 };

            int nRet = poll( &fd, 1, nTimeoutMs );
            if ( nRet < 0 )
            {
                if ( errno == EINTR )
                    continue;
                break;
            }
            if ( nRet == 0 )
                break; // Reader stalled; abandon the transfer.

            ssize_t nWritten = write( nFd, sText.data() + nOffset, sText.size() - nOffset );
            if ( nWritten < 0 )
            {
                if ( errno == EINTR || errno == EAGAIN )
                    continue;
                break;
            }
            nOffset += size_t( nWritten );
        }

        close( nFd );
    }

    bool CClipboardLoopGuard::ShouldPushToHost( const std::string &sText )
    {
        std::scoped_lock lock{ m_Mutex };

        // It came from the host, or we already pushed it. Either way the host
        // clipboard already holds this text.
        if ( m_oLastAccepted == sText || m_oLastPushed == sText )
            return false;

        m_oLastPushed = sText;
        m_oLastAccepted.reset();
        return true;
    }

    bool CClipboardLoopGuard::ShouldAcceptFromHost( const std::string &sText )
    {
        std::scoped_lock lock{ m_Mutex };

        // Our own push echoing back, or a repeat of what we already took.
        if ( m_oLastPushed == sText || m_oLastAccepted == sText )
            return false;

        m_oLastAccepted = sText;
        m_oLastPushed.reset();
        return true;
    }

    void CClipboardLoopGuard::Reset()
    {
        std::scoped_lock lock{ m_Mutex };
        m_oLastPushed.reset();
        m_oLastAccepted.reset();
    }
}
