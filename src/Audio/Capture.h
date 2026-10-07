// Game audio capture: one private PipeWire sink per gamescope session, so the
// game's sound lands in ONE place instead of being guessed at.
//
// What it is (superdoc/features/audio-capture.md has the full story and the
// Why: notes):
//   * A loopback pair on its own libpipewire thread. A capture stream announced
//     as `media.class = Audio/Sink` ("gamescope-ritz (game)") that games can
//     play into, and a playback stream to the system's DEFAULT output, which
//     follows the default when it changes. The samples pass through our own
//     code in between (Capture.cpp's playback_process(), the DSP hook point).
//   * A session TAG in the game's environment (PIPEWIRE_PROPS / PULSE_PROP:
//     `gamescope.ritz.session=<pid>`), injected by ApplyGameEnvironment()
//     before the game is launched.
//   * A MOVER: every Stream/Output/Audio node carrying our tag is moved into
//     the sink through the `default` metadata (`target.object` = the sink's
//     object.serial). Tagging, not target pinning: a user's own PipeWire rule
//     that strips `target.object` from a client's props cannot defeat it.
//   * Volume and mute on the sink node itself (pw_stream_set_control), so the
//     Mixer, `wpctl` and pavucontrol all see one value.
//
// A failure at any point (no PipeWire, a stream error) leaves everything as it
// was: Active() is false, StatusText() says why, and the Mixer's stream guessing
// (Volume.h) keeps working. All of it compiles to no-ops without libpipewire.
//
// Threading: every function below is safe to call from any thread, and none of
// the getters ever blocks on PipeWire (they read a mutex-guarded snapshot the
// libpipewire thread keeps current), so the overlay may call them per frame.
//
// This header deliberately includes no PipeWire header: the pure string
// helpers at the top are what tests/test_audio_capture.cpp exercises.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <unistd.h>

namespace gamescope::Audio::Capture
{
	// ---- Pure helpers (header-only so the unit tests need no PipeWire) -------

	// The property key the game's streams are tagged with. Its value is
	// SessionId().
	inline constexpr const char *kSessionKey = "gamescope.ritz.session";
	// The one extra property both env vars carry beside the tag: it stops
	// WirePlumber persisting OUR move as the game's remembered target, which
	// would otherwise leak out of gamescope (the user's CS2 -> EasyEffects
	// routing outside gamescope would silently change).
	inline constexpr const char *kRestoreTargetKey = "state.restore-target";

	// gamescope's own pid, as a string. A pid, not the app id: it needs no
	// sanitising (the app id is an opaque string) and two sessions on one
	// machine can never collide.
	inline std::string SessionId() { return std::to_string( (long long)getpid() ); }

	// The sink's node.name for a session id (the playback side is this + ".out").
	inline std::string SinkNameFor( std::string_view svSessionId )
	{
		return "gamescope-ritz." + std::string( svSessionId );
	}

	// ---- The playback copy, safe by construction ------------------------------
	//
	// What the playback stream's process callback does for each planar-float
	// channel (Capture.cpp's PlaybackProcess(), the DSP hook point). Pure, so
	// the unit tests can pin every case offline.
	//
	// Why it is this defensive: the copy runs on the realtime thread straight
	// into the user's speakers. Forwarding a buffer verbatim when the other side
	// has nothing valid (no buffer yet, a chunk flagged empty/corrupt, a size
	// that overruns maxsize, an offset past it) puts stale or uninitialised
	// memory on the output, which is a full-scale burst. So:
	//   * the source range is clamped to the source's maxsize (an offset past
	//     maxsize reads as "nothing valid");
	//   * a chunk flagged EMPTY or CORRUPTED, or no source at all, reads as silence;
	//   * a short source is zero-padded up to the wanted size;
	//   * the wanted size is clamped to the destination's maxsize and floored to
	//     whole floats, so nothing is written outside the buffer;
	//   * the last step clamps every output sample to [-1, 1] and turns NaN into 0,
	//     so no input, however broken, can exceed full scale.
	struct PlaneSource
	{
		const uint8_t *pData = nullptr;   // the source plane's mapped data (null = no source)
		uint32_t uMaxSize = 0;            // spa_data::maxsize
		uint32_t uOffset = 0;             // spa_chunk::offset
		uint32_t uSize = 0;               // spa_chunk::size
		bool bEmptyOrCorrupt = false;     // spa_chunk::flags & (EMPTY | CORRUPTED)
	};

	// Writes `uWantBytes` (clamped to uDstMaxSize, floored to whole floats) of
	// audio to pDst and returns the number of bytes written, which is what the
	// caller must put in the destination chunk's size. pSrc may be null.
	inline uint32_t CopyPlaneSafe( uint8_t *pDst, uint32_t uDstMaxSize, uint32_t uWantBytes, const PlaneSource *pSrc )
	{
		if ( !pDst )
			return 0;
		const uint32_t uOut = ( ( uWantBytes < uDstMaxSize ? uWantBytes : uDstMaxSize ) / 4 ) * 4;

		uint32_t uValid = 0;
		if ( pSrc && pSrc->pData && !pSrc->bEmptyOrCorrupt && pSrc->uOffset < pSrc->uMaxSize )
		{
			const uint32_t uAvail = pSrc->uMaxSize - pSrc->uOffset;
			uValid = pSrc->uSize < uAvail ? pSrc->uSize : uAvail;
			uValid = ( uValid < uOut ? uValid : uOut ) / 4 * 4;
		}

		if ( uValid )
			memcpy( pDst, pSrc->pData + pSrc->uOffset, uValid );
		if ( uOut > uValid )
			memset( pDst + uValid, 0, uOut - uValid );

		// DSP hook (Phase B): per-channel float samples in pDst[0, uOut), after the
		// copy and before the final clamp below.

		for ( uint32_t i = 0; i < uOut; i += 4 )
		{
			float f;
			uint32_t u;
			memcpy( &u, pDst + i, 4 );
			// NaN/inf are tested on the bits: the project builds with -ffast-math,
			// under which a float `f == f` test may be compiled away.
			if ( ( u & 0x7f800000u ) == 0x7f800000u )
				f = 0.0f;
			else
				memcpy( &f, &u, 4 );
			if ( f > 1.0f )
				f = 1.0f;
			else if ( f < -1.0f )
				f = -1.0f;
			memcpy( pDst + i, &f, 4 );
		}
		return uOut;
	}

	namespace Env
	{
		inline bool Contains( std::string_view svHay, std::string_view svNeedle )
		{
			return svHay.find( svNeedle ) != std::string_view::npos;
		}

		inline std::string_view Trim( std::string_view sv )
		{
			while ( !sv.empty() && ( sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\n' ) )
				sv.remove_prefix( 1 );
			while ( !sv.empty() && ( sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\n' ) )
				sv.remove_suffix( 1 );
			return sv;
		}

		// PIPEWIRE_PROPS is one SPA-JSON object: `{ key = value key2 = "v" }`.
		// Never overwrites what the launcher set: the launcher's pairs are kept
		// verbatim and ours are added beside them.
		//   * unset/empty            -> a fresh object holding our two keys
		//   * `{ ... }`              -> ours inserted before the closing brace
		//   * a bare `k = v ...` list (PipeWire's parser accepts it with the
		//     braces missing) -> wrapped in braces, ours appended
		//   * already has our tag    -> returned unchanged (idempotent; an outer
		//     gamescope's tag is left standing)
		// A launcher-set `state.restore-target` is honoured, not duplicated.
		inline std::string MergePipewireProps( std::string_view svExisting, std::string_view svSessionId )
		{
			const std::string sTag = std::string( kSessionKey ) + " = " + std::string( svSessionId );
			const std::string sRestore = std::string( kRestoreTargetKey ) + " = false";

			std::string_view svBody = Trim( svExisting );
			if ( Contains( svBody, kSessionKey ) )
				return std::string( svExisting );

			if ( svBody.size() >= 2 && svBody.front() == '{' && svBody.back() == '}' )
				svBody = Trim( svBody.substr( 1, svBody.size() - 2 ) );

			std::string sOut = "{ ";
			if ( !svBody.empty() )
			{
				sOut += svBody;
				sOut += ' ';
			}
			sOut += sTag;
			sOut += ' ';
			if ( !Contains( svBody, kRestoreTargetKey ) )
			{
				sOut += sRestore;
				sOut += ' ';
			}
			sOut += '}';
			return sOut;
		}

		// PULSE_PROP is space-separated `key=value` pairs. Ours are appended;
		// the launcher's string is kept verbatim, and a key it already carries
		// is not repeated.
		inline std::string AppendPulseProp( std::string_view svExisting, std::string_view svSessionId )
		{
			std::string sOut( Trim( svExisting ) );
			if ( Contains( sOut, kSessionKey ) )
				return std::string( svExisting );

			auto fnAdd = [ & ]( const std::string &sPair )
			{
				if ( !sOut.empty() )
					sOut += ' ';
				sOut += sPair;
			};
			fnAdd( std::string( kSessionKey ) + "=" + std::string( svSessionId ) );
			if ( !Contains( sOut, kRestoreTargetKey ) )
				fnAdd( std::string( kRestoreTargetKey ) + "=false" );
			return sOut;
		}
	}

	// ---- Environment injection ------------------------------------------------

	// Tags this process's own environment (which every child inherits) with the
	// session: PIPEWIRE_PROPS and PULSE_PROP are merged, never overwritten.
	// Done ALWAYS (it is cheap and inert), so capture can be switched on live
	// mid-session and still find the game's streams. PULSE_SINK is set to the
	// sink's name only as a fallback: only when `bCaptureWanted`, and only when
	// the launcher has not set one itself (the ritz launcher has a `sink`
	// field). Call before the game is launched. No-op without PipeWire.
	void ApplyGameEnvironment( bool bCaptureWanted );

	// ---- Lifecycle ----------------------------------------------------------------

	// The per-profile switch (`audio.capture`, default on). Safe to call at any
	// time, any number of times: before Start() it only records the wish;
	// afterwards it creates or destroys the sink live. Destroying it clears the
	// metadata we wrote, so the game's streams fall back to the default sink.
	void SetWanted( bool bWanted );
	bool GetWanted();

	// Starts the libpipewire thread, and, if capture is wanted, brings the sink
	// up (waiting up to ~1.5 s for it, so it exists before the game's first
	// stream does). Idempotent. Call BEFORE launching the game. A failure is
	// not an error to the caller: it is recorded in StatusText().
	void Start();

	// Stops the thread and removes everything of ours from the graph. Safe to
	// call more than once, and without Start(). (A SIGKILLed gamescope needs no
	// cleanup: PipeWire removes a dead client's nodes itself.)
	void Shutdown();

	// ---- State, for the Mixer's Diagnostics and Game volume row ----------------

	// True while the sink exists and is usable: the loopback pair is up and
	// the sink's object.serial is known, so tagged streams can be (and are)
	// moved into it. False when switched off, not started, or failed.
	bool Active();

	// One short human-readable line saying why capture is or isn't active:
	//   "Off (switched off in this profile)"   "Starting"
	//   "Active (gamescope-ritz.1234, 2 streams)"
	//   "Unavailable: PipeWire is not reachable"   "Error: <stream error>"
	//   "Unavailable: built without PipeWire"
	std::string StatusText();

	// How many tagged streams are currently moved into the sink (0 when
	// inactive). A live count of "the game's streams", not a guess.
	int StreamCount();

	// The sink's `node.name` ("gamescope-ritz.<pid>", usable as PULSE_SINK /
	// target.object by name), its PipeWire node id and its `object.serial`.
	// Empty / 0 until Active().
	std::string SinkName();
	uint32_t SinkNodeId();
	uint64_t SinkSerial();
	// The node id of OUR playback stream (the sink's other half, which feeds the
	// default output). It is a Stream/Output/Audio node like the game's, so a
	// stream list built from `wpctl status` shows it: filter it out. 0 until Active().
	uint32_t PlaybackNodeId();

	// ---- Volume and mute (act on the sink node) ----------------------------------
	//
	// Volume is in the Mixer's DISPLAY curve (Audio::DisplayToLinearVolume, the
	// cubic one pavucontrol/wpctl-style sliders show): 1.0 is unity, up to 1.5.
	// SetVolume() writes the matching channelVolumes on the sink, so `wpctl`
	// and pavucontrol see the same value; external changes come back into
	// GetVolume()/GetMute(). The setters always record the wish, even before
	// Start() or while the sink is switched off: it is applied the moment a sink
	// is ready, before any sample flows, and survives a live off/on (a recreated
	// sink would otherwise start at unity).
	void SetVolume( float flDisplayFraction );
	float GetVolume();
	void SetMute( bool bMute );
	bool GetMute();
}
