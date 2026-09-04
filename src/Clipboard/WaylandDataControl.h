// The data-control half of clipboard sync.
//
// `ext_data_control_v1` and `zwlr_data_control_unstable_v1` are the two
// protocols that let a client watch and set the host compositor's clipboard
// without holding keyboard focus. They are the same protocol -- ext- is the
// standardised rename of the wlr- one -- and are request-for-request and
// event-for-event identical for everything we use, so the implementation here
// is written once against a traits struct and stamped out for both.
//
// This is what makes clipboard sync an *event hook* rather than a poll: the
// `selection` event fires the moment the host clipboard changes, whether or
// not gamescope is focused. See superdoc/features/clipboard-sync.md.

#pragma once

#include "Clipboard/ClipboardSync.h"
#include "log.hpp"

#include <cstring>
#include <fcntl.h>
#include <functional>
#include <thread>
#include <unordered_map>

#include "wlr_begin.hpp"
#include <wayland-client.h>
#include <ext-data-control-v1-client-protocol.h>
#include <wlr-data-control-unstable-v1-client-protocol.h>
#include "wlr_end.hpp"

#include <unistd.h>

namespace gamescope
{
#define GAMESCOPE_DATA_CONTROL_TRAITS( NAME, PREFIX, DESC )                                                  \
    struct NAME                                                                                              \
    {                                                                                                        \
        using Manager        = PREFIX##_manager_v1;                                                          \
        using Device         = PREFIX##_device_v1;                                                           \
        using Offer          = PREFIX##_offer_v1;                                                            \
        using Source         = PREFIX##_source_v1;                                                           \
        using DeviceListener = PREFIX##_device_v1_listener;                                                  \
        using OfferListener  = PREFIX##_offer_v1_listener;                                                   \
        using SourceListener = PREFIX##_source_v1_listener;                                                  \
                                                                                                             \
        static constexpr const char *pszName = DESC;                                                         \
        static const wl_interface  *ManagerInterface() { return &PREFIX##_manager_v1_interface; }             \
                                                                                                             \
        static Device *GetDevice( Manager *pManager, wl_seat *pSeat )                                        \
            { return PREFIX##_manager_v1_get_data_device( pManager, pSeat ); }                                \
        static Source *CreateSource( Manager *pManager )                                                     \
            { return PREFIX##_manager_v1_create_data_source( pManager ); }                                    \
                                                                                                             \
        static void AddDeviceListener( Device *pDevice, const DeviceListener *pListener, void *pData )       \
            { PREFIX##_device_v1_add_listener( pDevice, pListener, pData ); }                                 \
        static void SetSelection( Device *pDevice, Source *pSource )                                         \
            { PREFIX##_device_v1_set_selection( pDevice, pSource ); }                                         \
        static void DestroyDevice( Device *pDevice )                                                         \
            { PREFIX##_device_v1_destroy( pDevice ); }                                                        \
                                                                                                             \
        static void AddOfferListener( Offer *pOffer, const OfferListener *pListener, void *pData )           \
            { PREFIX##_offer_v1_add_listener( pOffer, pListener, pData ); }                                   \
        static void Receive( Offer *pOffer, const char *pMime, int nFd )                                     \
            { PREFIX##_offer_v1_receive( pOffer, pMime, nFd ); }                                              \
        static void DestroyOffer( Offer *pOffer )                                                            \
            { PREFIX##_offer_v1_destroy( pOffer ); }                                                          \
                                                                                                             \
        static void AddSourceListener( Source *pSource, const SourceListener *pListener, void *pData )       \
            { PREFIX##_source_v1_add_listener( pSource, pListener, pData ); }                                 \
        static void SourceOffer( Source *pSource, const char *pMime )                                        \
            { PREFIX##_source_v1_offer( pSource, pMime ); }                                                   \
        static void DestroySource( Source *pSource )                                                         \
            { PREFIX##_source_v1_destroy( pSource ); }                                                        \
    };

    GAMESCOPE_DATA_CONTROL_TRAITS( CExtDataControlTraits,  ext_data_control,  "ext_data_control_v1" )
    GAMESCOPE_DATA_CONTROL_TRAITS( CWlrDataControlTraits, zwlr_data_control, "zwlr_data_control_manager_v1" )

#undef GAMESCOPE_DATA_CONTROL_TRAITS

    // A live data-control device: watches the host clipboard and can take
    // ownership of it. All Wayland traffic happens on the thread that
    // dispatches the display -- the compositor thread -- so every pipe
    // transfer is handed to a short-lived worker instead.
    template <typename Traits>
    class CDataControlDevice
    {
    public:
        using Manager = typename Traits::Manager;
        using Device  = typename Traits::Device;
        using Offer   = typename Traits::Offer;
        using Source  = typename Traits::Source;

        // fnOnText is called on a worker thread with the host's new clipboard
        // contents. It must be cheap and thread-safe -- in practice it drops
        // the string into a mailbox the compositor thread drains.
        void Init( wl_display *pDisplay, Manager *pManager, wl_seat *pSeat, std::function<void( std::string )> fnOnText )
        {
            m_pDisplay = pDisplay;
            m_pManager = pManager;
            m_fnOnText = std::move( fnOnText );
            m_pDevice  = Traits::GetDevice( pManager, pSeat );
            Traits::AddDeviceListener( m_pDevice, &s_DeviceListener, this );
        }

        bool IsValid() const { return m_pDevice != nullptr; }
        static const char *Name() { return Traits::pszName; }

        // Take ownership of the host clipboard and offer sText on it.
        void SetSelection( std::string sText )
        {
            if ( !m_pDevice )
                return;

            m_sOutgoing = std::move( sText );

            // The old source is not destroyed here: the compositor sends
            // `cancelled` when it drops it, and destroying it before that
            // would race a `send` already in flight.
            Source *pSource = Traits::CreateSource( m_pManager );
            Traits::AddSourceListener( pSource, &s_SourceListener, this );
            for ( const char *pMime : k_pszClipboardMimes )
                Traits::SourceOffer( pSource, pMime );

            m_pSource = pSource;
            Traits::SetSelection( m_pDevice, pSource );
            wl_display_flush( m_pDisplay );
        }

        void Shutdown()
        {
            for ( auto &Pair : m_Offers )
                Traits::DestroyOffer( Pair.first );
            m_Offers.clear();

            if ( m_pSource )
            {
                Traits::DestroySource( m_pSource );
                m_pSource = nullptr;
            }
            if ( m_pDevice )
            {
                Traits::DestroyDevice( m_pDevice );
                m_pDevice = nullptr;
            }
        }

    private:
        struct OfferState
        {
            int nBestRank = 0;
            std::string sBestMime;
        };

        void OnDataOffer( Device *pDevice, Offer *pOffer )
        {
            Traits::AddOfferListener( pOffer, &s_OfferListener, this );
            m_Offers[ pOffer ] = OfferState{};
        }

        void OnOfferMime( Offer *pOffer, const char *pMime )
        {
            auto it = m_Offers.find( pOffer );
            if ( it == m_Offers.end() )
                return;

            int nRank = RankClipboardMime( pMime );
            if ( nRank > it->second.nBestRank )
            {
                it->second.nBestRank = nRank;
                it->second.sBestMime = pMime;
            }
        }

        void OnSelection( Device *pDevice, Offer *pOffer )
        {
            if ( !pOffer )
                return; // Host clipboard cleared. Leave ours alone.

            auto it = m_Offers.find( pOffer );
            std::string sMime = it != m_Offers.end() ? it->second.sBestMime : std::string{};

            if ( !sMime.empty() )
                StartReceive( pOffer, sMime );

            DropOffer( pOffer );
        }

        void OnPrimarySelection( Device *pDevice, Offer *pOffer )
        {
            // PRIMARY is deliberately out of scope -- see the 2026-09-04
            // decision in superdoc/planning/requests-2026-09-04.md.
            if ( pOffer )
                DropOffer( pOffer );
        }

        void OnFinished( Device *pDevice )
        {
            // The compositor revoked the device (another data-control client
            // took over, or the seat went away). Stop touching it.
            Shutdown();
        }

        void DropOffer( Offer *pOffer )
        {
            m_Offers.erase( pOffer );
            Traits::DestroyOffer( pOffer );
        }

        void StartReceive( Offer *pOffer, const std::string &sMime )
        {
            int nPipe[ 2 ];
            if ( pipe2( nPipe, O_CLOEXEC ) != 0 )
                return;

            Traits::Receive( pOffer, sMime.c_str(), nPipe[ 1 ] );
            close( nPipe[ 1 ] );

            // The compositor has to see the receive request before anyone can
            // write to the pipe, and we are about to stop dispatching.
            wl_display_flush( m_pDisplay );

            // Read on a worker: the writer is an arbitrary host application,
            // and waiting on it here would stall frame pacing.
            std::thread( [ nFd = nPipe[ 0 ], fnOnText = m_fnOnText ]()
            {
                std::optional<std::string> osText = ReadClipboardPipe( nFd, k_nClipboardTransferTimeoutMs );
                if ( osText && fnOnText )
                    fnOnText( std::move( *osText ) );
            } ).detach();
        }

        void OnSourceSend( Source *pSource, const char *pMime, int nFd )
        {
            // Likewise a worker: the reader is a host application which may
            // not read for an arbitrarily long time.
            std::thread( [ nFd, sText = m_sOutgoing ]()
            {
                WriteClipboardPipe( nFd, sText, k_nClipboardTransferTimeoutMs );
            } ).detach();
        }

        void OnSourceCancelled( Source *pSource )
        {
            if ( m_pSource == pSource )
                m_pSource = nullptr;
            Traits::DestroySource( pSource );
        }

#define GAMESCOPE_DC_THUNK( name ) \
    []< typename... Args >( void *pData, Args... args ) { ( (CDataControlDevice *)pData )->name( std::forward<Args>( args )... ); }

        static inline const typename Traits::DeviceListener s_DeviceListener =
        {
            .data_offer        = GAMESCOPE_DC_THUNK( OnDataOffer ),
            .selection         = GAMESCOPE_DC_THUNK( OnSelection ),
            .finished          = GAMESCOPE_DC_THUNK( OnFinished ),
            .primary_selection = GAMESCOPE_DC_THUNK( OnPrimarySelection ),
        };

        static inline const typename Traits::OfferListener s_OfferListener =
        {
            .offer = GAMESCOPE_DC_THUNK( OnOfferMime ),
        };

        static inline const typename Traits::SourceListener s_SourceListener =
        {
            .send      = GAMESCOPE_DC_THUNK( OnSourceSend ),
            .cancelled = GAMESCOPE_DC_THUNK( OnSourceCancelled ),
        };

#undef GAMESCOPE_DC_THUNK

        wl_display *m_pDisplay = nullptr;
        Manager    *m_pManager = nullptr;
        Device     *m_pDevice  = nullptr;
        Source     *m_pSource  = nullptr;

        std::string m_sOutgoing;
        std::function<void( std::string )> m_fnOnText;
        std::unordered_map<Offer *, OfferState> m_Offers;
    };
}
