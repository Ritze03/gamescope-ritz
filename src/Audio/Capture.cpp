#include "Capture.h"

#include "Volume.h"
#include "../log.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#endif

static LogScope s_CaptureLog( "audio_capture" );

namespace gamescope::Audio::Capture
{
	// ---- The snapshot every getter reads ----------------------------------------
	//
	// Kept current by the libpipewire thread (and, optimistically, by the
	// volume/mute setters). Getters take only this mutex, never the
	// thread-loop lock, so a per-frame UI read can never block on PipeWire.
	namespace
	{
		struct Snapshot
		{
			bool bWanted = true;     // config default (audio.capture)
			bool bActive = false;
			std::string sStatus = "Off (not started)";
			std::string sSinkName;
			uint32_t uNodeId = 0;
			uint32_t uPlaybackNodeId = 0;
			uint64_t uSerial = 0;
			int nStreams = 0;
			std::string sRoute;      // node.name our playback is pointed at ("" = default output)
			float flVolume = 1.0f;   // display curve: what the sink reports now
			bool bMute = false;
			// What the caller asked for, kept across a live off/on (a recreated sink
			// starts at unity) and applied the moment a sink is ready, BEFORE any
			// sample flows. Only the setters write these; control_info never does.
			float flWantVolume = -1.0f;   // < 0 = never asked
			bool bWantMute = false;
		};

		std::mutex s_SnapMutex;
		Snapshot s_Snap;

		// Start()/Shutdown()/the setters serialise on this; getters never take it.
		std::mutex s_LifeMutex;
	}

	std::string SinkName() { std::lock_guard lock( s_SnapMutex ); return s_Snap.sSinkName; }
	uint32_t SinkNodeId() { std::lock_guard lock( s_SnapMutex ); return s_Snap.uNodeId; }
	uint32_t PlaybackNodeId() { std::lock_guard lock( s_SnapMutex ); return s_Snap.uPlaybackNodeId; }
	std::string PlaybackRoute() { std::lock_guard lock( s_SnapMutex ); return s_Snap.bActive ? s_Snap.sRoute : std::string{}; }
	uint64_t SinkSerial() { std::lock_guard lock( s_SnapMutex ); return s_Snap.uSerial; }
	bool Active() { std::lock_guard lock( s_SnapMutex ); return s_Snap.bActive; }
	std::string StatusText() { std::lock_guard lock( s_SnapMutex ); return s_Snap.sStatus; }
	int StreamCount() { std::lock_guard lock( s_SnapMutex ); return s_Snap.bActive ? s_Snap.nStreams : 0; }
	float GetVolume() { std::lock_guard lock( s_SnapMutex ); return s_Snap.flVolume; }
	bool GetMute() { std::lock_guard lock( s_SnapMutex ); return s_Snap.bMute; }
	bool GetWanted() { std::lock_guard lock( s_SnapMutex ); return s_Snap.bWanted; }

#if !HAVE_PIPEWIRE

	void ApplyGameEnvironment( bool ) {}
	void SetWanted( bool bWanted )
	{
		std::lock_guard lock( s_SnapMutex );
		s_Snap.bWanted = bWanted;
	}
	void Start()
	{
		std::lock_guard lock( s_SnapMutex );
		s_Snap.sStatus = "Unavailable: built without PipeWire";
	}
	void Shutdown() {}
	void SetVolume( float ) {}
	void SetMute( bool ) {}

#else // HAVE_PIPEWIRE

	namespace
	{
		constexpr uint32_t kMaxReasserts = 10;       // per stream: a fight with another writer ends here
		constexpr int kStartWaitMs = 1500;           // how long Start() waits for the sink to appear
		constexpr uint32_t kMaxVolumeChannels = SPA_AUDIO_MAX_CHANNELS;

		// The `target.object` the mover writes: the sink's object.serial, never
		// `target.node`, which loses to a writer (EasyEffects) that sets
		// `target.object`.
		constexpr const char *kTargetKey = "target.object";
		constexpr const char *kTargetNodeKey = "target.node";

		struct Impl;

		// One bound Stream/Output/Audio or Audio/Sink node whose full props
		// we are waiting for (the registry `global` event carries only a
		// subset of a node's props, never our custom ones).
		struct NodeWatch
		{
			Impl *pImpl = nullptr;
			uint32_t uId = 0;
			bool bSink = false;
			pw_node *pNode = nullptr;
			spa_hook listener{};
		};

		struct Tagged
		{
			uint32_t nReasserts = 0;
			bool bMoved = false;
			// The stream's own node props, as first seen: where it asked to go
			// (the original route's source (b)). Raw values; resolved when needed.
			std::string sPropObject;   // target.object
			std::string sPropNode;     // node.target
			bool bRouteTried = false;  // the original route was read for this stream
		};

		struct Impl
		{
			std::string sSession;
			std::string sSinkName;
			std::string sPlayName;
			std::string sGroup;

			pw_thread_loop *pLoop = nullptr;
			pw_context *pContext = nullptr;
			pw_core *pCore = nullptr;
			spa_hook coreListener{};
			pw_registry *pRegistry = nullptr;
			spa_hook regListener{};
			pw_metadata *pMeta = nullptr;
			spa_hook metaListener{};
			uint32_t uMetaId = SPA_ID_INVALID;

			int nSyncSeq = 0;
			int nSyncsDone = 0;
			bool bReady = false;        // initial registry + node infos are in
			bool bFailed = false;       // connection lost
			bool bWanted = true;
			bool bSettled = false;      // Start() may stop waiting

			// The loopback pair.
			pw_stream *pCap = nullptr;
			pw_stream *pPlay = nullptr;
			spa_hook capListener{};
			spa_hook playListener{};
			pw_stream_state eCapState = PW_STREAM_STATE_UNCONNECTED;
			pw_stream_state ePlayState = PW_STREAM_STATE_UNCONNECTED;
			std::string sPairError;
			uint32_t uChannels = 2;

			// The sink's identity as the registry reports it.
			uint32_t uSinkId = SPA_ID_INVALID;
			uint64_t uSinkSerial = 0;
			uint32_t uPlayId = SPA_ID_INVALID;
			std::string sSinkSerialStr;

			// Mirrors module-loopback.c: the two streams forward their latency to each other.
			spa_process_latency_info processLatency{};
			spa_latency_info latency[2]{};

			std::string sDefaultSink;                  // from the metadata's default.audio.sink
			std::map<uint32_t, SinkInfo> sinks;        // node id -> name, audio.position, serial (never ours)
			std::map<uint32_t, std::pair<uint32_t, uint32_t>> links;   // link id -> (output node, input node)
			// Every node's target entries in the `default` metadata (raw values), so
			// where a stream was headed can be read at the moment we move it.
			std::map<uint32_t, std::string> metaTargetObject;   // subject -> target.object
			std::map<uint32_t, std::string> metaTargetNode;     // subject -> target.node

			// The ORIGINAL ROUTE: the node.name of the sink the game's audio was
			// headed to before we moved it ("" = the default output). Our playback
			// is pointed there.
			// ponytail: ONE route per session. Several tagged streams with
			// different routes (a game playing some sounds to a headset and some to
			// the speakers) share the most recently recorded one; honouring each
			// would need one playback stream per route.
			std::string sRoute;
			std::string sPlayWritten;        // the target.object value we wrote for our playback ("" = none)
			uint32_t nPlayReasserts = 0;
			std::map<uint32_t, NodeWatch *> watches;   // every bound node
			std::map<uint32_t, Tagged> tagged;         // our session's streams

			bool bActive = false;
		};

		Impl *s_pImpl = nullptr;

		// ---- snapshot publishing ------------------------------------------------

		void Publish( Impl *p )
		{
			std::string sStatus;
			if ( p->bFailed )
				sStatus = "Unavailable: PipeWire connection lost";
			else if ( !p->bWanted )
				sStatus = "Off (switched off in this profile)";
			else if ( !p->sPairError.empty() )
				sStatus = "Error: " + p->sPairError;
			else if ( !p->pMeta && p->bReady )
				sStatus = "Unavailable: no PipeWire metadata (is WirePlumber running?)";
			else if ( p->bActive )
				sStatus = "Active (" + p->sSinkName + ")";
			else
				sStatus = "Starting";

			int nMoved = 0;
			for ( const auto &[ id, t ] : p->tagged )
				nMoved += t.bMoved ? 1 : 0;

			std::lock_guard lock( s_SnapMutex );
			s_Snap.bActive = p->bActive;
			s_Snap.sStatus = sStatus;
			s_Snap.sSinkName = p->bActive ? p->sSinkName : std::string{};
			s_Snap.uNodeId = p->bActive ? p->uSinkId : 0;
			s_Snap.uPlaybackNodeId = p->bActive && p->uPlayId != SPA_ID_INVALID ? p->uPlayId : 0;
			s_Snap.uSerial = p->bActive ? p->uSinkSerial : 0;
			s_Snap.nStreams = nMoved;
			s_Snap.sRoute = Route::PlaybackTargetFor( p->sRoute, p->sSinkName );
		}

		// ---- the mover ----------------------------------------------------------

		// Our playback's `target.object`, from the recorded route. With no route
		// nothing is written (and what we wrote is removed): WirePlumber then links
		// us to the default output and follows default changes. Never our own sink.
		// `bForce` rewrites an unchanged value (the re-assert).
		void ApplyPlaybackRoute( Impl *p, bool bForce = false )
		{
			if ( !p->pMeta || p->uPlayId == SPA_ID_INVALID )
				return;
			const std::string sWant = Route::PlaybackTargetFor( p->sRoute, p->sSinkName );
			if ( sWant.empty() )
			{
				if ( !p->sPlayWritten.empty() )
					pw_metadata_set_property( p->pMeta, p->uPlayId, kTargetKey, nullptr, nullptr );
				p->sPlayWritten.clear();
				return;
			}
			if ( sWant == p->sPlayWritten && !bForce )
				return;
			if ( sWant != p->sPlayWritten )
				p->nPlayReasserts = 0;
			// By node.name, as a string: it survives a restart of the target (a
			// serial does not). The spike found both forms work.
			pw_metadata_set_property( p->pMeta, p->uPlayId, kTargetKey, "Spa:String", sWant.c_str() );
			p->sPlayWritten = sWant;
			s_CaptureLog.infof( "playback -> %s", sWant.c_str() );
		}

		void SetRoute( Impl *p, const std::string &sRoute )
		{
			if ( p->sRoute == sRoute )
				return;
			s_CaptureLog.infof( "original route: %s", sRoute.empty() ? "(default output)" : sRoute.c_str() );
			p->sRoute = sRoute;
			ApplyPlaybackRoute( p );
			Publish( p );
		}

		// A raw target value of `uSubject` in the metadata -> a sink's node.name.
		std::string ResolveMetaTarget( Impl *p, uint32_t uSubject )
		{
			auto itObj = p->metaTargetObject.find( uSubject );
			if ( itObj != p->metaTargetObject.end() )
			{
				std::string s = Route::ResolveRouteValue( itObj->second, false, p->sinks );
				if ( !s.empty() )
					return s;
			}
			auto itNode = p->metaTargetNode.find( uSubject );
			return itNode != p->metaTargetNode.end() ? Route::ResolveRouteValue( itNode->second, true, p->sinks ) : std::string{};
		}

		// The sink a stream is linked to right now, if any (ours is not in `sinks`).
		std::string LinkedSinkOf( Impl *p, uint32_t uStreamId )
		{
			for ( const auto &[ id, io ] : p->links )
			{
				if ( io.first != uStreamId )
					continue;
				auto it = p->sinks.find( io.second );
				if ( it != p->sinks.end() )
					return it->second.sName;
			}
			return {};
		}

		// Reads where the stream was headed BEFORE our first write for it
		// (metadata entry, else its own props, else its link) and records that as
		// the route when it names one. Once per stream: after our move every one of
		// those sources points at us.
		void ObserveOriginalRoute( Impl *p, uint32_t uStreamId, Tagged &t )
		{
			if ( t.bRouteTried )
				return;
			t.bRouteTried = true;

			Route::Observation obs;
			obs.sMetaTarget = ResolveMetaTarget( p, uStreamId );
			obs.sPropTarget = Route::ResolveRouteValue( t.sPropObject, false, p->sinks );
			if ( obs.sPropTarget.empty() )
				obs.sPropTarget = Route::ResolveRouteValue( t.sPropNode, true, p->sinks );
			obs.sLinkedSink = LinkedSinkOf( p, uStreamId );
			const std::string sRoute = Route::PickInitialRoute( obs, p->sSinkName, p->sDefaultSink );
			if ( !sRoute.empty() )
				SetRoute( p, sRoute );
		}

		void MoveStream( Impl *p, uint32_t uStreamId, Tagged &t )
		{
			if ( !p->pMeta || p->sSinkSerialStr.empty() )
				return;
			ObserveOriginalRoute( p, uStreamId, t );
			pw_metadata_set_property( p->pMeta, uStreamId, kTargetKey, "Spa:Id", p->sSinkSerialStr.c_str() );
			t.bMoved = true;
		}

		void MoveAllTagged( Impl *p )
		{
			for ( auto &[ id, t ] : p->tagged )
				MoveStream( p, id, t );
		}

		// Drop every metadata entry we wrote, so the streams fall back to the
		// default sink. Done BEFORE the sink goes away.
		void ClearMoves( Impl *p )
		{
			if ( !p->pMeta )
				return;
			for ( auto &[ id, t ] : p->tagged )
			{
				if ( t.bMoved )
					pw_metadata_set_property( p->pMeta, id, kTargetKey, nullptr, nullptr );
				t.bMoved = false;
				t.nReasserts = 0;
				t.bRouteTried = false;
			}
			// Our playback's own entry too: it names a route that is no longer ours
			// to keep, and the node is about to go.
			if ( p->uPlayId != SPA_ID_INVALID )
				pw_metadata_set_property( p->pMeta, p->uPlayId, kTargetKey, nullptr, nullptr );
			p->sPlayWritten.clear();
			p->nPlayReasserts = 0;
			p->sRoute.clear();
		}

		// Writes the wished volume/mute onto the capture stream. Loop thread (lock
		// held) or under the thread-loop lock.
		void ApplyWants( Impl *p )
		{
			if ( !p->pCap )
				return;
			float flWantVolume;
			bool bWantMute;
			{
				std::lock_guard lock( s_SnapMutex );
				flWantVolume = s_Snap.flWantVolume;
				bWantMute = s_Snap.bWantMute;
			}
			if ( flWantVolume >= 0.0f )
			{
				const uint32_t nChannels = std::min( p->uChannels, kMaxVolumeChannels );
				float values[ kMaxVolumeChannels ];
				for ( uint32_t i = 0; i < nChannels; i++ )
					values[ i ] = DisplayToLinearVolume( flWantVolume );
				const int nRes = pw_stream_set_control( p->pCap, SPA_PROP_channelVolumes, nChannels, values, 0 );
				s_CaptureLog.debugf( "apply volume %.3f (state %s): %d", flWantVolume, pw_stream_state_as_string( p->eCapState ), nRes );
			}
			if ( bWantMute )
			{
				float flMute = 1.0f;
				const int nRes = pw_stream_set_control( p->pCap, SPA_PROP_mute, 1, &flMute, 0 );
				s_CaptureLog.debugf( "apply mute (state %s): %d", pw_stream_state_as_string( p->eCapState ), nRes );
			}
		}

		// Active = the sink exists and the mover can work: the capture stream is
		// ready, its serial is known, and the metadata is bound.
		void UpdateActive( Impl *p )
		{
			const bool bWasActive = p->bActive;
			const bool bCapReady = p->eCapState == PW_STREAM_STATE_PAUSED || p->eCapState == PW_STREAM_STATE_STREAMING;
			p->bActive = p->bWanted && p->pCap && bCapReady && p->uSinkSerial != 0 && p->pMeta && p->sPairError.empty();
			if ( p->bActive && !bWasActive )
			{
				s_CaptureLog.infof( "sink %s is up (node %u, serial %llu)", p->sSinkName.c_str(), p->uSinkId, (unsigned long long)p->uSinkSerial );
				ApplyWants( p );
				MoveAllTagged( p );
			}
			if ( !p->bActive )
			{
				for ( auto &[ id, t ] : p->tagged )
					t.bMoved = false;
			}
			if ( p->bActive || !p->bWanted || !p->sPairError.empty() || p->bFailed )
				p->bSettled = true;
			Publish( p );
			pw_thread_loop_signal( p->pLoop, false );
		}

		// ---- the loopback pair --------------------------------------------------

		// Capture side: its process callback only triggers the playback side in
		// the same graph cycle (like module-loopback), so the pair adds no extra quantum.
		void CaptureProcess( void *data )
		{
			Impl *p = static_cast<Impl *>( data );
			if ( pw_stream_trigger_process( p->pPlay ) < 0 )
			{
				pw_buffer *pBuf;
				while ( ( pBuf = pw_stream_dequeue_buffer( p->pCap ) ) )
					pw_stream_queue_buffer( p->pCap, pBuf );
			}
		}

		// Playback side: takes the newest captured buffer and copies it into its
		// own. THE DSP HOOK POINT: whatever should happen to the game's audio
		// (Phase B's compressor/limiter, a later per-channel sample hook) goes
		// into CopyPlaneSafe() (Capture.h), between the copy and its final clamp,
		// on the realtime thread.
		void PlaybackProcess( void *data )
		{
			Impl *p = static_cast<Impl *>( data );

			pw_buffer *pIn = nullptr, *pTmp;
			while ( ( pTmp = pw_stream_dequeue_buffer( p->pCap ) ) )
			{
				if ( pIn )
					pw_stream_queue_buffer( p->pCap, pIn );
				pIn = pTmp;
			}
			pw_buffer *pOut = pw_stream_dequeue_buffer( p->pPlay );

			if ( pOut )
			{
				// How much to produce: what the graph asked this cycle for
				// (`requested`, in frames), else the size the capture side delivered.
				// Every plane then goes through CopyPlaneSafe(): clamped to both
				// buffers, silence for anything missing/empty/corrupt/short, and a
				// final hard clamp to [-1, 1] -- never a buffer forwarded as found.
				const spa_buffer *pOutBuf = pOut->buffer;
				const spa_buffer *pInBuf = pIn ? pIn->buffer : nullptr;

				uint32_t uWant = 0;
				if ( pOut->requested > 0 )
				{
					uWant = (uint32_t)SPA_MIN( pOut->requested, (uint64_t)UINT32_MAX / sizeof( float ) ) * (uint32_t)sizeof( float );
				}
				else if ( pInBuf )
				{
					for ( uint32_t i = 0; i < pInBuf->n_datas; i++ )
						uWant = SPA_MAX( uWant, pInBuf->datas[ i ].chunk ? pInBuf->datas[ i ].chunk->size : 0u );
				}

				for ( uint32_t i = 0; i < pOutBuf->n_datas; i++ )
				{
					spa_data *dst = &pOutBuf->datas[ i ];
					if ( !dst->data || !dst->chunk )
						continue;

					PlaneSource src;
					if ( pInBuf && i < pInBuf->n_datas )
					{
						const spa_data *s = &pInBuf->datas[ i ];
						if ( s->data && s->chunk )
						{
							src.pData = static_cast<const uint8_t *>( s->data );
							src.uMaxSize = s->maxsize;
							src.uOffset = s->chunk->offset;
							src.uSize = s->chunk->size;
							src.bEmptyOrCorrupt = ( s->chunk->flags & ( SPA_CHUNK_FLAG_EMPTY | SPA_CHUNK_FLAG_CORRUPTED ) ) != 0;
						}
					}

					const uint32_t uWrote = CopyPlaneSafe( static_cast<uint8_t *>( dst->data ), dst->maxsize, uWant, &src );
					dst->chunk->offset = 0;
					dst->chunk->size = uWrote;
					dst->chunk->stride = sizeof( float );
					dst->chunk->flags = SPA_CHUNK_FLAG_NONE;
				}
			}

			if ( pIn )
				pw_stream_queue_buffer( p->pCap, pIn );
			if ( pOut )
				pw_stream_queue_buffer( p->pPlay, pOut );
		}

		// -- latency: each side forwards the other's, as module-loopback's update_latency() --

		void UpdateLatency( Impl *p, spa_direction eDirection, bool bProps, bool bProcess )
		{
			pw_stream *pStream = eDirection == SPA_DIRECTION_OUTPUT ? p->pPlay : p->pCap;
			if ( !pStream )
				return;

			uint8_t buf[ 1024 ];
			spa_pod_builder b;
			spa_pod_builder_init( &b, buf, sizeof( buf ) );
			const spa_pod *params[ 3 ];
			uint32_t nParams = 0;

			spa_latency_info latency = p->latency[ eDirection ];
			spa_process_latency_info_add( &p->processLatency, &latency );
			params[ nParams++ ] = spa_latency_build( &b, SPA_PARAM_Latency, &latency );

			if ( bProps )
			{
				params[ nParams++ ] = (const spa_pod *)spa_pod_builder_add_object( &b,
					SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
					SPA_PROP_latencyOffsetNsec, SPA_POD_Long( p->processLatency.ns ) );
			}
			if ( bProcess )
				params[ nParams++ ] = spa_process_latency_build( &b, SPA_PARAM_ProcessLatency, &p->processLatency );

			pw_stream_update_params( pStream, params, nParams );
		}

		void UpdateLatencies( Impl *p, bool bProps, bool bProcess )
		{
			UpdateLatency( p, SPA_DIRECTION_INPUT, bProps, bProcess );
			UpdateLatency( p, SPA_DIRECTION_OUTPUT, bProps, bProcess );
		}

		void ParamChanged( Impl *p, uint32_t uId, const spa_pod *pParam, pw_stream *pOther )
		{
			switch ( uId )
			{
				case SPA_PARAM_Latency:
				{
					spa_latency_info latency;
					if ( !pParam || spa_latency_parse( pParam, &latency ) < 0 )
						return;
					p->latency[ latency.direction ] = latency;
					UpdateLatency( p, latency.direction, false, false );
					break;
				}
				case SPA_PARAM_ProcessLatency:
				{
					spa_process_latency_info info;
					if ( !pParam )
						spa_zero( info );
					else if ( spa_process_latency_parse( pParam, &info ) < 0 )
						return;
					if ( spa_process_latency_info_compare( &p->processLatency, &info ) == 0 )
						return;
					p->processLatency = info;
					UpdateLatencies( p, true, true );
					break;
				}
				case SPA_PARAM_Props:
				{
					int64_t nNs = 0;
					if ( pParam && spa_pod_parse_object( pParam, SPA_TYPE_OBJECT_Props, nullptr,
					                                      SPA_PROP_latencyOffsetNsec, SPA_POD_Long( &nNs ) ) < 0 )
						return;
					if ( p->processLatency.ns == nNs )
						return;
					p->processLatency.ns = nNs;
					UpdateLatencies( p, true, true );
					break;
				}
				case SPA_PARAM_Tag:
				{
					if ( pParam && pOther )
					{
						const spa_pod *params[ 1 ] = { pParam };
						pw_stream_update_params( pOther, params, 1 );
					}
					break;
				}
				default:
					break;
			}
		}

		void CapParamChanged( void *data, uint32_t uId, const spa_pod *pParam )
		{
			Impl *p = static_cast<Impl *>( data );
			ParamChanged( p, uId, pParam, p->pPlay );
		}

		void PlayParamChanged( void *data, uint32_t uId, const spa_pod *pParam )
		{
			Impl *p = static_cast<Impl *>( data );
			ParamChanged( p, uId, pParam, p->pCap );
		}

		void PairStateChanged( Impl *p, const char *pszWho, pw_stream_state eOld, pw_stream_state eNew, const char *pszError )
		{
			s_CaptureLog.debugf( "%s stream: %s -> %s%s%s", pszWho, pw_stream_state_as_string( eOld ), pw_stream_state_as_string( eNew ),
				pszError ? " : " : "", pszError ? pszError : "" );
			if ( eNew == PW_STREAM_STATE_ERROR )
			{
				p->sPairError = std::string( pszWho ) + " stream: " + ( pszError ? pszError : "unknown error" );
				s_CaptureLog.errorf( "%s", p->sPairError.c_str() );
			}
			if ( eNew == PW_STREAM_STATE_PAUSED )
			{
				pw_stream_flush( p->pPlay, false );
				pw_stream_flush( p->pCap, false );
			}
		}

		void CapStateChanged( void *data, pw_stream_state eOld, pw_stream_state eNew, const char *pszError )
		{
			Impl *p = static_cast<Impl *>( data );
			p->eCapState = eNew;
			PairStateChanged( p, "capture", eOld, eNew, pszError );
			// The wished volume goes on at every step up to streaming: a control set
			// too early may be dropped, and it must be in place before samples flow.
			if ( eNew == PW_STREAM_STATE_PAUSED || eNew == PW_STREAM_STATE_STREAMING )
				ApplyWants( p );
			UpdateActive( p );
		}

		void PlayStateChanged( void *data, pw_stream_state eOld, pw_stream_state eNew, const char *pszError )
		{
			Impl *p = static_cast<Impl *>( data );
			p->ePlayState = eNew;
			PairStateChanged( p, "playback", eOld, eNew, pszError );
			UpdateActive( p );
		}

		// A volume or mute change from anywhere (our setter, wpctl, pavucontrol)
		// lands here and is mirrored into the snapshot.
		void CapControlInfo( void *, uint32_t uId, const pw_stream_control *pControl )
		{
			if ( !pControl || pControl->n_values == 0 )
				return;
			std::lock_guard lock( s_SnapMutex );
			if ( uId == SPA_PROP_channelVolumes )
			{
				float flMax = 0.0f;
				for ( uint32_t i = 0; i < pControl->n_values; i++ )
					flMax = std::max( flMax, pControl->values[ i ] );
				s_Snap.flVolume = LinearToDisplayVolume( flMax );
			}
			else if ( uId == SPA_PROP_mute )
			{
				s_Snap.bMute = pControl->values[ 0 ] >= 0.5f;
			}
		}

		const pw_stream_events s_CapEvents = {
			.version = PW_VERSION_STREAM_EVENTS,
			.state_changed = CapStateChanged,
			.control_info = CapControlInfo,
			.param_changed = CapParamChanged,
			.process = CaptureProcess,
		};

		const pw_stream_events s_PlayEvents = {
			.version = PW_VERSION_STREAM_EVENTS,
			.state_changed = PlayStateChanged,
			.param_changed = PlayParamChanged,
			.process = PlaybackProcess,
		};

		// The channel layout the pair is built with: the default output's, read
		// once when the pair is created.
		// ponytail: a live change of the default output's layout (stereo ->
		// 5.1 headset) is not followed until capture is switched off and on;
		// PipeWire up/downmixes the difference in the meantime.
		void PickLayout( Impl *p, spa_audio_info_raw *pInfo, std::string *psPosition )
		{
			uint32_t position[ SPA_AUDIO_MAX_CHANNELS ] = {};
			uint32_t nChannels = 0;
			for ( const auto &[ id, s ] : p->sinks )
			{
				if ( s.sName != p->sDefaultSink || s.sName == p->sSinkName || s.sPosition.empty() )
					continue;
				if ( spa_audio_parse_position( s.sPosition.c_str(), s.sPosition.size(), position, &nChannels ) >= 0 && nChannels > 0 )
				{
					*psPosition = s.sPosition;
					break;
				}
				nChannels = 0;
			}
			if ( nChannels == 0 )
			{
				nChannels = 2;
				position[ 0 ] = SPA_AUDIO_CHANNEL_FL;
				position[ 1 ] = SPA_AUDIO_CHANNEL_FR;
				*psPosition = "FL,FR";
			}
			spa_zero( *pInfo );
			pInfo->format = SPA_AUDIO_FORMAT_F32P;
			pInfo->channels = nChannels;
			for ( uint32_t i = 0; i < nChannels; i++ )
				pInfo->position[ i ] = position[ i ];
			p->uChannels = nChannels;
		}

		void DestroyPair( Impl *p )
		{
			p->bActive = false;
			ClearMoves( p );
			if ( p->pPlay )
			{
				spa_hook_remove( &p->playListener );
				pw_stream_destroy( p->pPlay );
				p->pPlay = nullptr;
			}
			if ( p->pCap )
			{
				spa_hook_remove( &p->capListener );
				pw_stream_destroy( p->pCap );
				p->pCap = nullptr;
			}
			p->eCapState = p->ePlayState = PW_STREAM_STATE_UNCONNECTED;
			p->uSinkId = p->uPlayId = SPA_ID_INVALID;
			p->uSinkSerial = 0;
			p->sSinkSerialStr.clear();
			p->sRoute.clear();
			p->sPlayWritten.clear();
			p->sPairError.clear();
			p->processLatency = {};
			p->latency[ 0 ] = p->latency[ 1 ] = {};
			Publish( p );
		}

		void CreatePair( Impl *p )
		{
			if ( p->pCap || p->pPlay || !p->pCore )
				return;

			spa_audio_info_raw info;
			std::string sPosition;
			PickLayout( p, &info, &sPosition );

			char szPid[ 32 ];
			snprintf( szPid, sizeof( szPid ), "%d", (int)getpid() );

			auto fnCommon = [ & ]( pw_properties *pProps )
			{
				// One group, and the same link-group: the link-group is what stops
				// WirePlumber linking our playback back into our own sink.
				pw_properties_set( pProps, PW_KEY_NODE_GROUP, p->sGroup.c_str() );
				pw_properties_set( pProps, PW_KEY_NODE_LINK_GROUP, p->sGroup.c_str() );
				pw_properties_set( pProps, PW_KEY_NODE_VIRTUAL, "true" );
				pw_properties_set( pProps, "audio.position", sPosition.c_str() );
				// Without these WirePlumber's state-stream restores a remembered
				// volume/target for OUR nodes.
				pw_properties_set( pProps, "state.restore-props", "false" );
				pw_properties_set( pProps, "state.restore-target", "false" );
				pw_properties_set( pProps, kSessionKey, p->sSession.c_str() );
				pw_properties_set( pProps, "gamescope.ritz.pid", szPid );
			};

			pw_properties *pCapProps = pw_properties_new(
				PW_KEY_NODE_NAME, p->sSinkName.c_str(),
				PW_KEY_NODE_DESCRIPTION, "gamescope-ritz (game)",
				PW_KEY_MEDIA_NAME, "gamescope-ritz game audio",
				PW_KEY_MEDIA_CLASS, "Audio/Sink",
				nullptr );
			pw_properties *pPlayProps = pw_properties_new(
				PW_KEY_NODE_NAME, p->sPlayName.c_str(),
				PW_KEY_NODE_DESCRIPTION, "gamescope-ritz (game) output",
				PW_KEY_MEDIA_NAME, "gamescope-ritz game audio output",
				PW_KEY_MEDIA_TYPE, "Audio",
				PW_KEY_MEDIA_CATEGORY, "Playback",
				"stream.dont-remix", "true",
				// No node.dont-move: our playback goes where the game's audio would
				// have gone without us (the original route, written into the
				// `default` metadata for this node by ApplyPlaybackRoute()); with
				// no route recorded it stays target-less, so WirePlumber links it
				// to the default output and follows default changes. EasyEffects'
				// "process all outputs" also writes a target for it, which is the
				// same sink whenever that is the game's route.
				nullptr );
			if ( !pCapProps || !pPlayProps )
			{
				if ( pCapProps ) pw_properties_free( pCapProps );
				if ( pPlayProps ) pw_properties_free( pPlayProps );
				p->sPairError = "out of memory";
				return;
			}
			fnCommon( pCapProps );
			fnCommon( pPlayProps );

			p->pCap = pw_stream_new( p->pCore, "gamescope-ritz capture", pCapProps );
			p->pPlay = pw_stream_new( p->pCore, "gamescope-ritz playback", pPlayProps );
			if ( !p->pCap || !p->pPlay )
			{
				p->sPairError = "pw_stream_new failed";
				DestroyPair( p );
				p->sPairError = "pw_stream_new failed";
				Publish( p );
				return;
			}
			pw_stream_add_listener( p->pCap, &p->capListener, &s_CapEvents, p );
			pw_stream_add_listener( p->pPlay, &p->playListener, &s_PlayEvents, p );

			uint8_t buf[ 1024 ];
			spa_pod_builder b;
			const spa_pod *params[ 1 ];

			// Playback first: it has to be activated before the capture side can
			// trigger it. Rate is left unset on purpose: the pair follows the
			// graph's rate (seen at 44.1 kHz/512 and 48 kHz/1024; it is dynamic).
			spa_pod_builder_init( &b, buf, sizeof( buf ) );
			params[ 0 ] = spa_format_audio_raw_build( &b, SPA_PARAM_EnumFormat, &info );
			int nRes = pw_stream_connect( p->pPlay, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				(pw_stream_flags)( PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_TRIGGER ),
				params, 1 );
			if ( nRes < 0 )
			{
				std::string sErr = std::string( "playback connect failed: " ) + spa_strerror( nRes );
				DestroyPair( p );
				p->sPairError = sErr;
				Publish( p );
				return;
			}

			spa_pod_builder_init( &b, buf, sizeof( buf ) );
			params[ 0 ] = spa_format_audio_raw_build( &b, SPA_PARAM_EnumFormat, &info );
			nRes = pw_stream_connect( p->pCap, PW_DIRECTION_INPUT, PW_ID_ANY,
				(pw_stream_flags)( PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_ASYNC ),
				params, 1 );
			if ( nRes < 0 )
			{
				std::string sErr = std::string( "capture connect failed: " ) + spa_strerror( nRes );
				DestroyPair( p );
				p->sPairError = sErr;
				Publish( p );
				return;
			}

			s_CaptureLog.infof( "created %s (%u channels: %s)", p->sSinkName.c_str(), p->uChannels, sPosition.c_str() );
			ApplyWants( p );
			UpdateActive( p );
		}

		// Creates the pair once everything it depends on is known (the registry
		// has been read, so the default output's layout is available).
		void MaybeCreatePair( Impl *p )
		{
			if ( p->bReady && p->bWanted && !p->pCap && !p->bFailed && p->sPairError.empty() )
				CreatePair( p );
			else
				UpdateActive( p );
		}

		// ---- metadata -----------------------------------------------------------

		// default.audio.sink's value is JSON: {"name":"alsa_output...."}.
		std::string ParseDefaultSinkName( const char *pszValue )
		{
			if ( !pszValue )
				return {};
			const char *pszKey = strstr( pszValue, "\"name\"" );
			if ( !pszKey )
				return {};
			const char *pszOpen = strchr( pszKey + 6, '"' );
			if ( !pszOpen )
				return {};
			const char *pszClose = strchr( pszOpen + 1, '"' );
			if ( !pszClose )
				return {};
			return std::string( pszOpen + 1, pszClose );
		}

		int MetaProperty( void *data, uint32_t uSubject, const char *pszKey, const char *, const char *pszValue )
		{
			Impl *p = static_cast<Impl *>( data );

			if ( uSubject == 0 && pszKey && !strcmp( pszKey, "default.audio.sink" ) )
			{
				p->sDefaultSink = ParseDefaultSinkName( pszValue );
				return 0;
			}

			if ( uSubject == 0 )
				return 0;

			// Remember every node's target entries (a clear, key == null, drops both):
			// where a stream was headed is read from here at the moment we move it.
			const bool bObject = pszKey && !strcmp( pszKey, kTargetKey );
			const bool bNode = pszKey && !strcmp( pszKey, kTargetNodeKey );
			if ( !pszKey || bObject )
			{
				if ( pszValue ) p->metaTargetObject[ uSubject ] = pszValue; else p->metaTargetObject.erase( uSubject );
			}
			if ( !pszKey || bNode )
			{
				if ( pszValue ) p->metaTargetNode[ uSubject ] = pszValue; else p->metaTargetNode.erase( uSubject );
			}
			if ( !p->bActive || ( pszKey && !bObject && !bNode ) )
				return 0;

			// Someone wrote a target for OUR playback (EasyEffects' "process all
			// outputs" does, for every output stream). Fight it only when we know where
			// we should go and it is a different sink (Route::ShouldReassertPlayback),
			// capped like the game streams' below.
			// ponytail: with no route recorded we do not fight at all; the default
			// output is only a guess at "where the game would have gone", and a
			// writer that states a target for us is as good a guess.
			if ( uSubject == p->uPlayId )
			{
				if ( !bObject )
					return 0;   // target.node loses to our target.object anyway
				const std::string sOurs = Route::PlaybackTargetFor( p->sRoute, p->sSinkName );
				const std::string sForeign = pszValue ? Route::ResolveRouteValue( pszValue, false, p->sinks ) : std::string{};
				if ( !Route::ShouldReassertPlayback( sOurs, sForeign ) )
					return 0;
				if ( p->nPlayReasserts >= kMaxReasserts )
				{
					if ( p->nPlayReasserts == kMaxReasserts )
						s_CaptureLog.warnf( "playback: giving up re-asserting its target after %u tries (another writer keeps changing it)", kMaxReasserts );
					p->nPlayReasserts++;
					return 0;
				}
				p->nPlayReasserts++;
				s_CaptureLog.debugf( "playback: target.object changed by someone else (%s), re-asserting (#%u)", sForeign.c_str(), p->nPlayReasserts );
				ApplyPlaybackRoute( p, true );
				return 0;
			}

			auto it = p->tagged.find( uSubject );
			if ( it == p->tagged.end() )
				return 0;

			// A foreign write for a game stream (not ours, and it names a known sink)
			// is where the game would have gone without us: the latest one wins.
			if ( pszValue )
			{
				const std::string sForeign = Route::ResolveRouteValue( pszValue, bNode, p->sinks );
				SetRoute( p, Route::RecordForeignWrite( p->sRoute, sForeign, p->sSinkName ) );
			}

			// EasyEffects (and anything else that writes target.object) races us:
			// put ours back whenever a tagged stream's value differs, a bounded
			// number of times so two writers can never fight forever.
			if ( bNode )
				return 0;   // target.node loses to our target.object, nothing to put back
			if ( pszKey && pszValue && ( strtoull( pszValue, nullptr, 10 ) == p->uSinkSerial || p->sSinkName == pszValue ) )
				return 0;   // ours, or already the right value
			if ( it->second.nReasserts >= kMaxReasserts )
			{
				if ( it->second.nReasserts == kMaxReasserts )
					s_CaptureLog.warnf( "node %u: giving up re-asserting its target after %u tries (another writer keeps changing it)", uSubject, kMaxReasserts );
				it->second.nReasserts++;
				return 0;
			}
			it->second.nReasserts++;
			s_CaptureLog.debugf( "node %u: target.object changed by someone else, re-asserting (#%u)", uSubject, it->second.nReasserts );
			MoveStream( p, uSubject, it->second );
			return 0;
		}

		const pw_metadata_events s_MetaEvents = {
			.version = PW_VERSION_METADATA_EVENTS,
			.property = MetaProperty,
		};

		// ---- registry -----------------------------------------------------------

		void NodeInfo( void *data, const pw_node_info *pInfo )
		{
			NodeWatch *w = static_cast<NodeWatch *>( data );
			Impl *p = w->pImpl;
			if ( !pInfo || !pInfo->props )
				return;

			if ( w->bSink )
			{
				const char *pszName = spa_dict_lookup( pInfo->props, PW_KEY_NODE_NAME );
				const char *pszPos = spa_dict_lookup( pInfo->props, "audio.position" );
				SinkInfo &s = p->sinks[ w->uId ];   // created with its serial at RegGlobal()
				if ( pszName )
					s.sName = pszName;
				s.sPosition = pszPos ? pszPos : "";
				return;
			}

			// A Stream/Output/Audio node. OUR playback carries our pid marker and
			// is never moved into our own sink; the game's streams carry only the tag.
			if ( spa_dict_lookup( pInfo->props, "gamescope.ritz.pid" ) )
				return;
			const char *pszTag = spa_dict_lookup( pInfo->props, kSessionKey );
			if ( !pszTag || p->sSession != pszTag )
				return;
			if ( p->tagged.count( w->uId ) )
				return;

			const char *pszApp = spa_dict_lookup( pInfo->props, PW_KEY_APP_NAME );
			s_CaptureLog.infof( "tagged stream: node %u (%s)", w->uId, pszApp ? pszApp : "?" );
			Tagged &t = p->tagged[ w->uId ];
			const char *pszPropObject = spa_dict_lookup( pInfo->props, kTargetKey );
			const char *pszPropNode = spa_dict_lookup( pInfo->props, "node.target" );
			t.sPropObject = pszPropObject ? pszPropObject : "";
			t.sPropNode = pszPropNode ? pszPropNode : "";
			if ( p->bActive )
				MoveStream( p, w->uId, t );
			Publish( p );
		}

		const pw_node_events s_NodeEvents = {
			.version = PW_VERSION_NODE_EVENTS,
			.info = NodeInfo,
		};

		void WatchNode( Impl *p, uint32_t uId, bool bSink )
		{
			NodeWatch *w = new NodeWatch();
			w->pImpl = p;
			w->uId = uId;
			w->bSink = bSink;
			w->pNode = static_cast<pw_node *>( pw_registry_bind( p->pRegistry, uId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0 ) );
			if ( !w->pNode )
			{
				delete w;
				return;
			}
			pw_node_add_listener( w->pNode, &w->listener, &s_NodeEvents, w );
			p->watches[ uId ] = w;
		}

		void UnwatchNode( NodeWatch *w )
		{
			spa_hook_remove( &w->listener );
			pw_proxy_destroy( reinterpret_cast<pw_proxy *>( w->pNode ) );
			delete w;
		}

		void RegGlobal( void *data, uint32_t uId, uint32_t, const char *pszType, uint32_t, const spa_dict *pProps )
		{
			Impl *p = static_cast<Impl *>( data );
			if ( !pProps )
				return;

			if ( !strcmp( pszType, PW_TYPE_INTERFACE_Metadata ) )
			{
				const char *pszName = spa_dict_lookup( pProps, PW_KEY_METADATA_NAME );
				if ( pszName && !strcmp( pszName, "default" ) && !p->pMeta )
				{
					p->pMeta = static_cast<pw_metadata *>( pw_registry_bind( p->pRegistry, uId, pszType, PW_VERSION_METADATA, 0 ) );
					if ( p->pMeta )
					{
						p->uMetaId = uId;
						pw_metadata_add_listener( p->pMeta, &p->metaListener, &s_MetaEvents, p );
					}
				}
				return;
			}

			if ( !strcmp( pszType, PW_TYPE_INTERFACE_Link ) )
			{
				// Only to answer "which sink is this stream linked to" (the original
				// route's last source).
				const char *pszOut = spa_dict_lookup( pProps, PW_KEY_LINK_OUTPUT_NODE );
				const char *pszIn = spa_dict_lookup( pProps, PW_KEY_LINK_INPUT_NODE );
				if ( pszOut && pszIn )
					p->links[ uId ] = { (uint32_t)strtoul( pszOut, nullptr, 10 ), (uint32_t)strtoul( pszIn, nullptr, 10 ) };
				return;
			}

			if ( strcmp( pszType, PW_TYPE_INTERFACE_Node ) != 0 )
				return;

			const char *pszClass = spa_dict_lookup( pProps, PW_KEY_MEDIA_CLASS );
			const char *pszName = spa_dict_lookup( pProps, PW_KEY_NODE_NAME );
			if ( !pszClass )
				return;

			if ( !strcmp( pszClass, "Audio/Sink" ) )
			{
				if ( pszName && p->sSinkName == pszName )
				{
					// OUR sink: the object.serial is what the mover points streams at.
					const char *pszSerial = spa_dict_lookup( pProps, PW_KEY_OBJECT_SERIAL );
					p->uSinkId = uId;
					p->uSinkSerial = pszSerial ? strtoull( pszSerial, nullptr, 10 ) : 0;
					p->sSinkSerialStr = pszSerial ? pszSerial : "";
					UpdateActive( p );
				}
				else
				{
					// Its serial is kept so a metadata value (a serial) resolves to a name.
					const char *pszSerial = spa_dict_lookup( pProps, PW_KEY_OBJECT_SERIAL );
					SinkInfo &s = p->sinks[ uId ];
					s.sName = pszName ? pszName : "";
					s.uSerial = pszSerial ? strtoull( pszSerial, nullptr, 10 ) : 0;
					WatchNode( p, uId, true );
				}
			}
			else if ( !strncmp( pszClass, "Stream/Output/Audio", 19 ) )
			{
				if ( pszName && p->sPlayName == pszName )
				{
					p->uPlayId = uId;
					ApplyPlaybackRoute( p );
					Publish( p );
				}
				else
				{
					// The global's props are only a subset, never our custom keys:
					// bind the node and read its full props in info().
					WatchNode( p, uId, false );
				}
			}
		}

		void RegGlobalRemove( void *data, uint32_t uId )
		{
			Impl *p = static_cast<Impl *>( data );
			auto it = p->watches.find( uId );
			if ( it != p->watches.end() )
			{
				UnwatchNode( it->second );
				p->watches.erase( it );
			}
			p->links.erase( uId );
			p->metaTargetObject.erase( uId );
			p->metaTargetNode.erase( uId );
			auto itSink = p->sinks.find( uId );
			if ( itSink != p->sinks.end() )
			{
				// The original route's node went away (EasyEffects quit): our playback's
				// target would point at nothing, so drop the route and fall back to the
				// default output. If the sink comes back, the next foreign write for a
				// game stream records it again.
				const bool bRoute = !p->sRoute.empty() && itSink->second.sName == p->sRoute;
				p->sinks.erase( itSink );
				if ( bRoute )
					SetRoute( p, "" );
			}
			if ( p->tagged.erase( uId ) )
				Publish( p );
			if ( uId == p->uMetaId )
			{
				spa_hook_remove( &p->metaListener );
				pw_proxy_destroy( reinterpret_cast<pw_proxy *>( p->pMeta ) );
				p->pMeta = nullptr;
				p->uMetaId = SPA_ID_INVALID;
				UpdateActive( p );
			}
			if ( uId == p->uPlayId )
			{
				p->uPlayId = SPA_ID_INVALID;
				Publish( p );
			}
		}

		const pw_registry_events s_RegEvents = {
			.version = PW_VERSION_REGISTRY_EVENTS,
			.global = RegGlobal,
			.global_remove = RegGlobalRemove,
		};

		// ---- core ---------------------------------------------------------------

		void CoreDone( void *data, uint32_t uId, int nSeq )
		{
			Impl *p = static_cast<Impl *>( data );
			if ( uId != PW_ID_CORE || nSeq != p->nSyncSeq )
				return;
			p->nSyncsDone++;
			// Round trip 1: every registry global is in and the nodes are bound.
			// Round trip 2: their info (names, audio.position) is in too.
			if ( p->nSyncsDone == 1 )
			{
				p->nSyncSeq = pw_core_sync( p->pCore, PW_ID_CORE, 0 );
			}
			else if ( p->nSyncsDone == 2 )
			{
				p->bReady = true;
				MaybeCreatePair( p );
				if ( !p->bWanted )
				{
					p->bSettled = true;
					Publish( p );
				}
			}
			else
			{
				// A round trip somebody asked for (see Flush()).
				p->bSettled = true;
			}
			pw_thread_loop_signal( p->pLoop, false );
		}

		void CoreError( void *data, uint32_t uId, int nSeq, int nRes, const char *pszMessage )
		{
			Impl *p = static_cast<Impl *>( data );
			s_CaptureLog.errorf( "core error on %u (seq %d): %s: %s", uId, nSeq, spa_strerror( nRes ), pszMessage ? pszMessage : "" );
			if ( uId == PW_ID_CORE && nRes == -EPIPE )
			{
				// ponytail: no reconnect. A PipeWire restart takes the game's own
				// streams down with it; capture stays off until gamescope restarts.
				p->bFailed = true;
				p->bActive = false;
				p->bSettled = true;
				Publish( p );
				pw_thread_loop_signal( p->pLoop, false );
			}
		}

		const pw_core_events s_CoreEvents = {
			.version = PW_VERSION_CORE_EVENTS,
			.done = CoreDone,
			.error = CoreError,
		};

		// Blocks (loop lock held by the caller) until everything we have sent so
		// far has been processed by the server, or the timeout passes.
		void Flush( Impl *p, int nTimeoutMs )
		{
			if ( !p->pCore || p->bFailed )
				return;
			p->bSettled = false;
			p->nSyncSeq = pw_core_sync( p->pCore, PW_ID_CORE, 0 );
			p->nSyncsDone = 2;   // so CoreDone() lands in its "asked for" branch
			const auto tEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds( nTimeoutMs );
			while ( !p->bSettled && !p->bFailed && std::chrono::steady_clock::now() < tEnd )
				pw_thread_loop_timed_wait( p->pLoop, 1 );
		}
	}

	// ---- public surface ----------------------------------------------------------

	void ApplyGameEnvironment( bool bCaptureWanted )
	{
		const std::string sId = SessionId();

		const char *pszPwProps = getenv( "PIPEWIRE_PROPS" );
		setenv( "PIPEWIRE_PROPS", Env::MergePipewireProps( pszPwProps ? pszPwProps : "", sId ).c_str(), 1 );

		const char *pszPulseProp = getenv( "PULSE_PROP" );
		setenv( "PULSE_PROP", Env::AppendPulseProp( pszPulseProp ? pszPulseProp : "", sId ).c_str(), 1 );

		// Fallback only, and never over a launcher's own choice.
		const char *pszPulseSink = getenv( "PULSE_SINK" );
		if ( bCaptureWanted && ( !pszPulseSink || !*pszPulseSink ) )
			setenv( "PULSE_SINK", SinkNameFor( sId ).c_str(), 1 );
	}

	void SetWanted( bool bWanted )
	{
		std::lock_guard life( s_LifeMutex );
		{
			std::lock_guard lock( s_SnapMutex );
			if ( s_Snap.bWanted == bWanted )
				return;   // also the common case: every config bump calls this
			s_Snap.bWanted = bWanted;
		}
		Impl *p = s_pImpl;
		if ( !p )
			return;

		pw_thread_loop_lock( p->pLoop );
		if ( p->bWanted != bWanted )
		{
			p->bWanted = bWanted;
			if ( bWanted )
			{
				p->sPairError.clear();
				MaybeCreatePair( p );
			}
			else
			{
				DestroyPair( p );
				// The metadata writes must reach the server before anything else
				// can notice the sink is gone.
				Flush( p, 500 );
			}
			Publish( p );
		}
		pw_thread_loop_unlock( p->pLoop );
	}

	void Start()
	{
		std::lock_guard life( s_LifeMutex );
		if ( s_pImpl )
			return;

		auto fnFail = [ & ]( const char *pszWhy )
		{
			s_CaptureLog.warnf( "%s; game audio capture is off for this session", pszWhy );
			std::lock_guard lock( s_SnapMutex );
			s_Snap.bActive = false;
			s_Snap.sStatus = std::string( "Unavailable: " ) + pszWhy;
		};

		pw_init( nullptr, nullptr );

		Impl *p = new Impl();
		p->sSession = SessionId();
		p->sSinkName = SinkNameFor( p->sSession );
		p->sPlayName = p->sSinkName + ".out";
		p->sGroup = "gamescope-ritz-" + p->sSession;
		{
			std::lock_guard lock( s_SnapMutex );
			p->bWanted = s_Snap.bWanted;
		}

		p->pLoop = pw_thread_loop_new( "gamescope-audio", nullptr );
		if ( !p->pLoop )
		{
			delete p;
			fnFail( "could not create the PipeWire loop" );
			return;
		}
		pw_properties *pCtxProps = pw_properties_new( PW_KEY_APP_NAME, "gamescope-ritz", nullptr );
		p->pContext = pw_context_new( pw_thread_loop_get_loop( p->pLoop ), pCtxProps, 0 );
		if ( !p->pContext )
		{
			pw_thread_loop_destroy( p->pLoop );
			delete p;
			fnFail( "could not create a PipeWire context" );
			return;
		}

		pw_thread_loop_lock( p->pLoop );
		if ( pw_thread_loop_start( p->pLoop ) < 0 )
		{
			pw_thread_loop_unlock( p->pLoop );
			pw_context_destroy( p->pContext );
			pw_thread_loop_destroy( p->pLoop );
			delete p;
			fnFail( "could not start the PipeWire thread" );
			return;
		}

		p->pCore = pw_context_connect( p->pContext, nullptr, 0 );
		if ( !p->pCore )
		{
			pw_thread_loop_unlock( p->pLoop );
			pw_thread_loop_stop( p->pLoop );
			pw_context_destroy( p->pContext );
			pw_thread_loop_destroy( p->pLoop );
			delete p;
			fnFail( "PipeWire is not reachable" );
			return;
		}
		pw_core_add_listener( p->pCore, &p->coreListener, &s_CoreEvents, p );
		p->pRegistry = pw_core_get_registry( p->pCore, PW_VERSION_REGISTRY, 0 );
		pw_registry_add_listener( p->pRegistry, &p->regListener, &s_RegEvents, p );
		p->nSyncSeq = pw_core_sync( p->pCore, PW_ID_CORE, 0 );
		s_pImpl = p;

		{
			std::lock_guard lock( s_SnapMutex );
			s_Snap.sStatus = p->bWanted ? "Starting" : "Off (switched off in this profile)";
		}

		// Wait for the sink so it exists before the game's first stream does.
		// Bounded: a missing session manager must not stall the launch.
		if ( p->bWanted )
		{
			const auto tEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds( kStartWaitMs );
			while ( !p->bSettled && !p->bFailed && std::chrono::steady_clock::now() < tEnd )
				pw_thread_loop_timed_wait( p->pLoop, 1 );
			if ( !p->bActive )
				s_CaptureLog.warnf( "the sink was not ready after %d ms (%s); it keeps trying in the background", kStartWaitMs, StatusText().c_str() );
		}
		pw_thread_loop_unlock( p->pLoop );
	}

	void Shutdown()
	{
		std::lock_guard life( s_LifeMutex );
		Impl *p = s_pImpl;
		if ( !p )
			return;
		s_pImpl = nullptr;

		pw_thread_loop_lock( p->pLoop );
		p->bWanted = false;
		DestroyPair( p );
		Flush( p, 500 );
		for ( auto &[ id, w ] : p->watches )
			UnwatchNode( w );
		p->watches.clear();
		if ( p->pMeta )
		{
			spa_hook_remove( &p->metaListener );
			pw_proxy_destroy( reinterpret_cast<pw_proxy *>( p->pMeta ) );
			p->pMeta = nullptr;
		}
		spa_hook_remove( &p->regListener );
		pw_proxy_destroy( reinterpret_cast<pw_proxy *>( p->pRegistry ) );
		spa_hook_remove( &p->coreListener );
		pw_core_disconnect( p->pCore );
		pw_thread_loop_unlock( p->pLoop );

		pw_thread_loop_stop( p->pLoop );
		pw_context_destroy( p->pContext );
		pw_thread_loop_destroy( p->pLoop );
		delete p;

		std::lock_guard lock( s_SnapMutex );
		s_Snap.bActive = false;
		s_Snap.sStatus = "Off (stopped)";
		s_Snap.sSinkName.clear();
		s_Snap.uNodeId = s_Snap.uPlaybackNodeId = 0;
		s_Snap.uSerial = 0;
		s_Snap.nStreams = 0;
	}

	// Volume/mute: always recorded as the wish (so a call before Start() or
	// during a live off/on is not lost), written onto the sink at once when it
	// is up, and otherwise applied by ApplyWants() the moment it becomes ready.
	void SetVolume( float flDisplayFraction )
	{
		flDisplayFraction = std::clamp( flDisplayFraction, 0.0f, 1.5f );
		{
			std::lock_guard lock( s_SnapMutex );
			s_Snap.flWantVolume = flDisplayFraction;
			s_Snap.flVolume = flDisplayFraction;
		}
		std::lock_guard life( s_LifeMutex );
		Impl *p = s_pImpl;
		if ( !p )
			return;
		pw_thread_loop_lock( p->pLoop );
		if ( p->bActive )
			ApplyWants( p );
		pw_thread_loop_unlock( p->pLoop );
	}

	void SetMute( bool bMute )
	{
		{
			std::lock_guard lock( s_SnapMutex );
			s_Snap.bWantMute = bMute;
			s_Snap.bMute = bMute;
		}
		std::lock_guard life( s_LifeMutex );
		Impl *p = s_pImpl;
		if ( !p )
			return;
		pw_thread_loop_lock( p->pLoop );
		if ( p->bActive && p->pCap )
		{
			float flMute = bMute ? 1.0f : 0.0f;
			pw_stream_set_control( p->pCap, SPA_PROP_mute, 1, &flMute, 0 );
		}
		pw_thread_loop_unlock( p->pLoop );
	}

#endif // HAVE_PIPEWIRE
}
