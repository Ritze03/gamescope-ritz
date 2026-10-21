// M5 Audio panel -- see PanelAudio.h and superdoc/planning/SPEC.md's
// Feature 5 ("PipeWire volume").
//
// This panel is deliberately thin: Audio::GetState() already hands back a
// fully-resolved snapshot (wpctl availability, detection method, matched/
// candidate counts, current display-fraction volume, mute) and the curve is
// handled inside src/Audio/Volume.cpp -- see Volume.h's "Volume curve"
// comment. The slider below reads/writes display-fraction units only and
// never re-applies a curve of its own (that would double-apply it).
//
// Two paths drive the "Game volume" row (superdoc/features/audio-mixer.md):
//   - CAPTURE (game audio capture is Active, src/Audio/Capture.h): the row
//     is Capture::SetVolume()/SetMute() on the private sink the game plays
//     into -- one control for everything the game plays, no guessing.
//   - FALLBACK (capture off, or it failed): the stream-guessing strategies
//     below (manual pick, PID tree, process name, newest stream), exactly as
//     before capture existed.
//
// Every "control isn't usable (yet)" state is explained, never just a
// silently-dead slider (DECISIONS.md #22/#23):
//   - wpctl missing entirely -> whole panel greyed, one status line.
//   - stream not detected -> slider/mute still drawn but disabled, with a
//     status line that says *why* (no audio anywhere yet, audio exists but
//     none of it matched, or a manual pick isn't currently live) -- never
//     just "not detected" with no further explanation.
//   - several candidates matched at once -> reported, not silently resolved
//     -- the most recently created one is used (Volume.h's SelectCandidate),
//     and the status line says so.
// The manual picker below is always available (not just as an error-state
// fallback) so a wrong automatic pick can be corrected regardless of why
// it's wrong.
#include "PanelAudio.h"

#include "../Audio/Capture.h"
#include "../Audio/Dynamics.h"
#include "../Audio/Volume.h"
#include "Config/ConfigManager.h"
#include "UI/Registry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		// Lazily-loaded, mutated only by the manual-picker buttons below,
		// and persisted via EnqueueRoutedWrite() -- global.json or the
		// current session's games/<AppId>.json snapshot, whichever
		// config::IsSessionOverrideActive() says is authoritative. Same
		// pattern as PanelDisplay.cpp's s_CachedSettings.
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_CachedSettings;

		// ---- Volume jump-back fix -------------------------------------
		// Audio::GetState()/GetAvailableStreams() are cheap snapshots of
		// the background poll thread's *last* 750ms-cadence tick (see
		// Volume.cpp) - between the instant a slider calls RequestVolume*()
		// and the next tick landing, every intervening frame's read of
		// state/candidate.flVolume is still the pre-change value. Reading
		// it fresh every frame (the old behavior) therefore made the
		// slider visibly snap back to the old value the instant the user
		// let go, then jump forward again once the poll thread caught up.
		//
		// Fix is UI-side optimistic local state, not a longer poll
		// interval (which would only make the same window wider, not go
		// away): remember the value each row just requested and when, and
		// keep displaying that instead of the polled value until either
		// the poll thread reports back a value that agrees (fastest path -
		// Volume.cpp also now applies a row's own command and re-reads it
		// back within that same poll tick, so this is often well under
		// 750ms) or a short timeout elapses (a safety net against a wpctl
		// call that silently failed). The poll thread was not also made to
		// suppress its own readback after applying a command - it already
		// applies a command and reads the result back synchronously
		// within one tick (Volume.cpp's PollThreadMain), so there is no
		// separate "still settling" race for it to guard against; the
		// only staleness is this UI-side one, between ticks.
		struct PendingRowState
		{
			bool bHasVolume = false;
			float flVolume = 0.0f;
			bool bHasMute = false;
			bool bMuted = false;
			std::chrono::steady_clock::time_point tWhen{};
		};

		constexpr std::chrono::milliseconds k_PendingTimeout{ 1000 };
		constexpr float k_flVolumeConfirmEpsilon = 0.005f; // ~0.5%, well under a whole UI percent

		PendingRowState s_PendingCapture; // the same, for the Game volume row while it drives the capture sink
		PendingRowState s_PendingPrimary; // the panel's first/"the game" slider - no stable node id from the UI's POV (see VolumeState::vecMatchedNodeIds), so kept separate from the per-node map below
		std::unordered_map<int, PendingRowState> s_PendingByNode; // every other stream row, keyed by its own node id

		// Returns what to display right now, clearing the override once
		// the poll thread's own value agrees or the timeout passes.
		float ResolveDisplayVolume( PendingRowState &pending, float flLiveVolume )
		{
			if ( !pending.bHasVolume )
				return flLiveVolume;

			const bool bConfirmed = std::abs( flLiveVolume - pending.flVolume ) < k_flVolumeConfirmEpsilon;
			if ( bConfirmed || ( std::chrono::steady_clock::now() - pending.tWhen ) > k_PendingTimeout )
			{
				pending.bHasVolume = false;
				return flLiveVolume;
			}
			return pending.flVolume;
		}

		bool ResolveDisplayMute( PendingRowState &pending, bool bLiveMuted )
		{
			if ( !pending.bHasMute )
				return bLiveMuted;

			if ( bLiveMuted == pending.bMuted || ( std::chrono::steady_clock::now() - pending.tWhen ) > k_PendingTimeout )
			{
				pending.bHasMute = false;
				return bLiveMuted;
			}
			return pending.bMuted;
		}

		// Profiles v2: the manual stream pick is a per-GAME fact, not a
		// setting -- it lives in global.json's games.<AppId>.audio_node
		// (config::GameEntry()), never in a profile, because it names one
		// game's process. With no app id it is session-only.
		std::string s_sManualNode;

		void PushManualSelectionToLiveState()
		{
			const std::string &sBinary = s_sManualNode;
			Audio::SetManualSelection( sBinary.empty() ? std::nullopt : std::optional<std::string>( sBinary ) );
		}

		void SaveManualNode()
		{
			if ( const std::optional<std::string> &oAppId = config::SessionAppId() )
				config::SetGameAudioNode( *oAppId, s_sManualNode );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;

			s_CachedSettings = config::ResolvedSettings();
			if ( const std::optional<std::string> &oAppId = config::SessionAppId() )
				s_sManualNode = config::GameEntry( *oAppId ).audio_node;
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;

			// Push on first load too, not just a reload -- Audio::Init()'s
			// background thread starts with no manual selection at all
			// until this runs once.
			PushManualSelectionToLiveState();
		}

		void QueueSave()
		{
			config::EnqueueRoutedWrite( s_CachedSettings );
		}

		const char *DetectionMethodLabel( Audio::DetectionMethod eMethod )
		{
			switch ( eMethod )
			{
				case Audio::DetectionMethod::Pid:         return "matched by process";
				case Audio::DetectionMethod::ProcessName: return "matched by process name (PID match failed -- likely a sandboxed launch)";
				case Audio::DetectionMethod::Newest:      return "guessed: newest audio stream since launch";
				case Audio::DetectionMethod::Manual:      return "manually selected";
				case Audio::DetectionMethod::NotDetected:
				default:                                  return "not detected";
			}
		}

		// The identity SelectCandidate()/SetManualSelection() key on --
		// application.process.binary, falling back to application.name.
		// Mirrors Volume.cpp's Parse::IdentityKey() exactly (kept in sync
		// by hand since that helper is file-local there).
		std::string CandidateIdentity( const Audio::StreamCandidate &candidate )
		{
			return !candidate.sBinary.empty() ? candidate.sBinary : candidate.sAppName;
		}
	}

	// =====================================================================
	//  P3 part B -- the E2 registration
	// =====================================================================
	// The area above, declared instead of drawn. Nothing about what it
	// CONTROLS changed: the same Audio:: calls, the same one config key
	// (audio.manual_node_binary), the same optimistic-pending layer that
	// fixed issue #36's jump-back. What changed is that the rows are now
	// registrations, so the Inspector derives their help, their provenance
	// and their reset from the same declaration the sheet row comes from.
	//
	// THE THING THAT MAKES THIS AREA DIFFERENT FROM EVERY OTHER ONE: its
	// rows are not known when RegisterAll() runs. A stream row exists
	// because an application is making a sound right now. See
	// ui::Area::Rebuilds() for the whole argument; the short version is
	// that a row's identity comes from the PipeWire node, never from its
	// position in a list, because a positional row silently retargets when
	// the stream above it ends.
	namespace
	{
		// ---- the per-frame snapshot -----------------------------------
		// Refreshed once per frame by AudioGeneration(), which the registry
		// calls before anything reads a row. Every binding below reads THIS
		// rather than calling Audio::GetState() itself, so all the rows in
		// one frame agree about what the audio server looked like.
		Audio::VolumeState                  s_AreaState;
		bool                                s_bCaptureActive = false;   // Audio::Capture::Active() this frame: which path Game volume drives
		std::vector<Audio::StreamCandidate> s_vecAreaStreams;

		// ui::Option holds a `const char *`, so the option labels need
		// storage that outlives the registration. A deque, not a vector:
		// push_back must not invalidate the pointers already handed out.
		std::deque<std::string> s_dqOptionText;
		std::vector<ui::Option> s_vecStreamOptions;
		std::vector<std::string> s_vecOptionIdentity;   // parallel; [0] is "" == automatic

		const Audio::StreamCandidate *FindStream( int nNodeId )
		{
			for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
				if ( c.nNodeId == nNodeId )
					return &c;
			return nullptr;
		}

		bool IsPrimaryNode( int nNodeId )
		{
			return std::find( s_AreaState.vecMatchedNodeIds.begin(),
			                  s_AreaState.vecMatchedNodeIds.end(), nNodeId )
			       != s_AreaState.vecMatchedNodeIds.end();
		}

		// The matched node the "Game volume" row's dynamic label is named
		// after, when there is more than one (see PrimaryStreamRowLabel()
		// below). s_vecAreaStreams is already sorted ascending by node id
		// (AudioGeneration() above), so "the first one found here" is the
		// LOWEST node id among the matched set -- stable across a poll tick
		// for a fixed candidate set, unlike the push order
		// SelectCandidate() builds vecMatchedNodeIds in (Volume.cpp), which
		// is not sorted at all. Several matched nodes always share one
		// winning IDENTITY (same application.process.binary/application.name
		// -- see SelectCandidate()), so in practice they carry the same
		// name anyway; which one stands in for the group barely matters.
		const Audio::StreamCandidate *PrimaryMatchedStream()
		{
			for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
				if ( IsPrimaryNode( c.nNodeId ) )
					return &c;
			return nullptr;
		}

		// The name a row is titled with. candidate.sLabel is ALREADY issue
		// #63's precedence chain -- application.name, then media.name, then
		// whatever `wpctl status` printed -- assembled in Volume.cpp's poll
		// thread. It is deliberately not re-derived here: #63 took that
		// precedence from ncpamixer's source, and a second copy of it would
		// be a second answer to the same question.
		std::string StreamName( const Audio::StreamCandidate &c )
		{
			return c.sLabel.empty() ? std::string( "(unnamed stream)" ) : c.sLabel;
		}

		// ---- the generation --------------------------------------------
		// A function of everything the ROW SET depends on and nothing else.
		// Note what is absent: flVolume and bMuted. A generation that moved
		// with the volume would rebuild the area under the pointer on every
		// drag, which is both a waste and a way to lose a drag mid-gesture.
		uint64_t AudioGeneration()
		{
			EnsureConfigLoaded();

			s_AreaState      = Audio::GetState();
			s_bCaptureActive = Audio::Capture::Active();
			// So the poll never lists or guesses our own capture nodes (also caught by name).
			Audio::SetOwnNodeIds( Audio::Capture::PlaybackNodeId(), Audio::Capture::SinkNodeId() );
			s_vecAreaStreams = Audio::GetAvailableStreams();
			std::sort( s_vecAreaStreams.begin(), s_vecAreaStreams.end(),
				[]( const Audio::StreamCandidate &a, const Audio::StreamCandidate &b )
				{
					return a.nNodeId < b.nNodeId;
				} );

			// Drop optimistic overrides for streams that no longer exist --
			// nothing is left to confirm them against, and the map would
			// otherwise grow all session. Same cleanup the legacy body does.
			std::erase_if( s_PendingByNode, []( const auto &kv )
			{
				return !FindStream( kv.first );
			} );

			uint64_t ulHash = 1469598103934665603ull;
			const auto Mix = [ &ulHash ]( std::string_view sv )
			{
				for ( unsigned char c : sv )
				{
					ulHash ^= c;
					ulHash *= 1099511628211ull;
				}
				ulHash ^= 0xff;
				ulHash *= 1099511628211ull;
			};

			Mix( s_AreaState.bWpctlAvailable ? "wpctl" : "no-wpctl" );
			// Which path the Game volume row drives changes the row set.
			Mix( s_bCaptureActive ? "capture" : "no-capture" );
			Mix( s_AreaState.bDetected ? "detected" : "undetected" );
			Mix( s_AreaState.sManualSelection );
			Mix( s_sManualNode );
			for ( int nId : s_AreaState.vecMatchedNodeIds )
				Mix( std::to_string( nId ) );
			for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
			{
				Mix( std::to_string( c.nNodeId ) );
				Mix( c.sLabel );      // a row's TITLE, so a rename is a rebuild
				Mix( c.sBinary );
			}
			return ulHash;
		}

		// ---- the bindings ----------------------------------------------
		// Every one of them goes through the SAME optimistic-pending layer
		// the legacy panel uses (ResolveDisplayVolume / ResolveDisplayMute
		// above). That layer is issue #36's fix: Audio's poll thread is on a
		// 750 ms cadence, so between letting go of a slider and the next
		// tick, a naive read still returns the OLD value and the slider
		// snaps back. Re-deriving that here rather than reusing it would
		// have reintroduced exactly the bug the brief warns about.
		ui::AnyBind BindNodeVolume( int nNodeId )
		{
			return ui::AnyBind::Of<int>(
				[ nNodeId ]() -> int
				{
					const Audio::StreamCandidate *pStream = FindStream( nNodeId );
					const float flLive = pStream ? pStream->flVolume : 0.0f;
					return (int)std::lround(
						ResolveDisplayVolume( s_PendingByNode[ nNodeId ], flLive ) * 100.0f );
				},
				[ nNodeId ]( int nPercent )
				{
					const float flFrac = std::clamp( nPercent, 0, 150 ) / 100.0f;
					Audio::RequestVolumeForNode( nNodeId, flFrac );
					PendingRowState &pending = s_PendingByNode[ nNodeId ];
					pending.bHasVolume = true;
					pending.flVolume   = flFrac;
					pending.tWhen      = std::chrono::steady_clock::now();
				} );
		}

		ui::AnyBind BindNodeMute( int nNodeId )
		{
			return ui::AnyBind::Of<bool>(
				[ nNodeId ]() -> bool
				{
					const Audio::StreamCandidate *pStream = FindStream( nNodeId );
					return ResolveDisplayMute( s_PendingByNode[ nNodeId ],
					                           pStream && pStream->bMuted );
				},
				[ nNodeId ]( bool bMuted )
				{
					Audio::RequestMuteForNode( nNodeId, bMuted );
					PendingRowState &pending = s_PendingByNode[ nNodeId ];
					pending.bHasMute = true;
					pending.bMuted   = bMuted;
					pending.tWhen    = std::chrono::steady_clock::now();
				} );
		}

		// ---- the manual picker's options -------------------------------
		void RebuildStreamOptions()
		{
			s_dqOptionText.clear();
			s_vecStreamOptions.clear();
			s_vecOptionIdentity.clear();

			s_dqOptionText.emplace_back( "Automatic" );
			s_vecStreamOptions.push_back( { 0, s_dqOptionText.back().c_str() } );
			s_vecOptionIdentity.emplace_back();

			for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
			{
				// A stream that reported neither a binary nor an
				// application name cannot be remembered by identity across
				// a relaunch, so it is not offerable as a manual pick. The
				// legacy picker drew it greyed with a tooltip; a Choice has
				// no per-option disable, so it is omitted here and the
				// count of omitted streams is reported as a Details fact
				// instead -- the information survives, in the place the
				// Inspector puts provenance.
				const std::string sIdentity = CandidateIdentity( c );
				if ( sIdentity.empty() )
					continue;

				s_dqOptionText.emplace_back( StreamName( c ) + "  (" + sIdentity + ")" );
				s_vecStreamOptions.push_back(
					{ (int)s_vecOptionIdentity.size(), s_dqOptionText.back().c_str() } );
				s_vecOptionIdentity.push_back( sIdentity );
			}

			// A manual override whose stream is not live right now still has
			// to be SHOWN, or the picker would read "Automatic" while an
			// override is in force -- and the next click would silently
			// clear a setting the user never touched.
			const std::string &sManual = s_sManualNode;
			if ( !sManual.empty() &&
			     std::find( s_vecOptionIdentity.begin(), s_vecOptionIdentity.end(), sManual )
			         == s_vecOptionIdentity.end() )
			{
				s_dqOptionText.emplace_back( sManual + "  (not streaming)" );
				s_vecStreamOptions.push_back(
					{ (int)s_vecOptionIdentity.size(), s_dqOptionText.back().c_str() } );
				s_vecOptionIdentity.push_back( sManual );
			}
		}

		int CurrentPickerIndex()
		{
			const std::string &sManual = s_sManualNode;
			if ( sManual.empty() )
				return 0;
			for ( size_t i = 0; i < s_vecOptionIdentity.size(); ++i )
				if ( s_vecOptionIdentity[ i ] == sManual )
					return (int)i;
			return 0;
		}

		std::string DetectionSummary()
		{
			if ( !s_AreaState.bWpctlAvailable )
				return "wpctl not found";
			if ( !s_AreaState.bDetected )
				return "no game stream matched";
			return DetectionMethodLabel( s_AreaState.eMethod );
		}

		// "Active (gamescope-ritz.1234), 2 streams" -- Capture's own status line, plus
		// how many of the game's streams are in the sink right now (only meaningful
		// while it is Active; every other status already says why it is not).
		std::string CaptureStatusLine()
		{
			std::string s = Audio::Capture::StatusText();
			if ( Audio::Capture::Active() )
			{
				const int n = Audio::Capture::StreamCount();
				s += ", " + std::to_string( n ) + ( n == 1 ? " stream" : " streams" );
			}
			return s;
		}

		// Which path the Game volume row is driving right now, in words.
		std::string GameVolumeDrives()
		{
			if ( s_bCaptureActive )
				return "the capture sink (" + Audio::Capture::SinkName() + ")";
			if ( s_AreaState.bWpctlAvailable && s_AreaState.bDetected )
				return std::string( "a guessed stream (" ) + DetectionMethodLabel( s_AreaState.eMethod ) + ")";
			return Audio::Capture::GetWanted()
				? "nothing yet (capture is not active, and no game stream was found)"
				: "nothing (capture is off, and no game stream was found)";
		}

		// The Game volume row, the same on both paths apart from what it is bound
		// to and how it explains itself. Its id never changes (the settings audit,
		// the palette's identity and anything else keyed on it are unaffected).
		void EmitGameVolumeRow( ui::Area &a, const std::string &sTitle, ui::AnyBind volume, ui::AnyBind mute, const char *pszHelp )
		{
			a.Slider( "audio.stream.volume", sTitle.c_str(), std::move( volume ) )
				.Help( pszHelp )
				.Range( 0.0f, 150.0f )
				// The binding is an int percent, so the range is 151 discrete
				// values -- over the 100-position budget on its own. 5% is
				// the granularity a volume control is normally read at, and
				// 100%, the default and the unity point, is on the grid.
				.Step( 5.0f )    // 31 positions
				.Default( 100 )
				.Unit( "%" )
				// "game volume" kept as a literal keyword (not just
				// the two words separately) because the row's own
				// TITLE -- which the palette also searches -- isn't
				// always "Game volume" any more (see
				// PrimaryStreamRowLabel() below): while Automatic
				// detection is showing a stream's own name instead,
				// a search for "game volume" would otherwise find
				// nothing here.
				.Keywords( "volume gain level loudness game audio game volume stream mixer detected capture" )
				.Param( "mute", "Mute", std::move( mute ) )
					.Help( "Mutes the sound without changing its volume." )
					.Default( false );
		}

		// The "Game volume" row's own title -- threads the live state
		// (s_sManualNode, the detected primary stream, how many other
		// nodes moved with it) into the pure decision in PanelAudio.h's
		// AudioMixerPrimaryRowLabel(), which is what tests/
		// test_audio_volume.cpp exercises directly. See that function's
		// own doc comment for the reasoning and what each case means.
		//
		// Entry::Emit() copies pszTitle into its own std::string
		// immediately (Registry.h's m_sTitle), so the string returned
		// here only has to live for the .Slider() call itself -- no
		// persistent/static storage needed, unlike the deque of C strings
		// RebuildStreamOptions() keeps for ui::Option (which DOES hold a
		// bare const char* across the frame).
		std::string PrimaryStreamRowLabel()
		{
			const Audio::StreamCandidate *pPrimary = s_sManualNode.empty() ? PrimaryMatchedStream() : nullptr;
			const std::string sPrimaryName = pPrimary ? pPrimary->sLabel : std::string();
			const size_t nOtherMatched = s_AreaState.vecMatchedNodeIds.empty()
				? 0 : s_AreaState.vecMatchedNodeIds.size() - 1;

			return AudioMixerPrimaryRowLabel( !s_sManualNode.empty(), sPrimaryName, nOtherMatched );
		}

		// ---- Dynamics rows ---------------------------------------------
		// audio.dynamics.* (per profile, default off), applied to the capture node
		// by main.cpp's ritz_apply_config_live() -> Capture::SetDynamics() like
		// audio.capture. The Preset row and the six Advanced sliders edit the same
		// AudioDynamicsSettings: picking a named preset loads its values into the
		// sliders; moving a slider makes the preset "custom" and keeps the values.
		constexpr ui::Option kDynamicsPresetOptions[] = {
			{ 0, "Off" }, { 1, "Even out" }, { 2, "Night" }, { 3, "Limiter only" }, { 4, "Custom" } };
		constexpr const char *kDynamicsPresetKeys[] = { "off", "even_out", "night", "limiter_only", "custom" };
		constexpr Audio::Dynamics::Preset kDynamicsPresets[] = {
			Audio::Dynamics::Preset::Off, Audio::Dynamics::Preset::EvenOut,
			Audio::Dynamics::Preset::Night, Audio::Dynamics::Preset::LimiterOnly };

		int DynamicsPresetIndex()
		{
			EnsureConfigLoaded();
			const std::string &s = s_CachedSettings.audio.dynamics.preset;
			for ( int i = 0; i < 5; i++ )
				if ( s == kDynamicsPresetKeys[ i ] )
					return i;
			return 0;
		}

		void SetDynamicsPreset( int n )
		{
			EnsureConfigLoaded();
			n = std::clamp( n, 0, 4 );
			auto &d = s_CachedSettings.audio.dynamics;
			d.preset = kDynamicsPresetKeys[ n ];
			if ( n >= 1 && n <= 3 )
			{
				const Audio::Dynamics::Params p = Audio::Dynamics::PresetParams( kDynamicsPresets[ n ] );
				d.threshold_db = p.flThresholdDb;
				d.ratio = p.flRatio;
				d.attack_ms = p.flAttackMs;
				d.release_ms = p.flReleaseMs;
				d.makeup_db = p.flMakeupDb;
				d.ceiling_db = p.flCeilingDb;
			}
			QueueSave();
		}

		// One Advanced slider's bind: reads the stored value, writes it and flips
		// the preset to Custom.
		ui::AnyBind DynamicsBind( float config::AudioDynamicsSettings::*pMember )
		{
			return ui::AnyBind::Of<float>(
				[ pMember ]{ EnsureConfigLoaded(); return s_CachedSettings.audio.dynamics.*pMember; },
				[ pMember ]( float f )
				{
					EnsureConfigLoaded();
					auto &d = s_CachedSettings.audio.dynamics;
					d.*pMember = f;
					d.preset = "custom";
					QueueSave();
				} );
		}

		bool CaptureOn() { return s_bCaptureActive; }

		// The Diagnostics line: what the stage is doing right now.
		std::string DynamicsStatusLine()
		{
			EnsureConfigLoaded();
			const Audio::Capture::DynamicsStatus st = Audio::Capture::GetDynamicsStatus();
			const std::string &sPreset = s_CachedSettings.audio.dynamics.preset;
			if ( sPreset == "off" )
				return "off";
			if ( !s_bCaptureActive )
				return "waiting for capture";
			if ( !st.uChannels )
				return "not available for this audio format";
			char sz[ 128 ];
			std::snprintf( sz, sizeof( sz ), "%s  ·  adds %.1f ms  ·  limiter backstop hits %llu",
				DynamicsPresetIndex() == 4 ? "Custom" : Audio::Dynamics::PresetName( kDynamicsPresets[ DynamicsPresetIndex() ] ),
				st.flLatencyMs, (unsigned long long)st.uBackstopHits );
			return sz;
		}

		// ---- the builder -----------------------------------------------
		void BuildAudioArea( ui::Area &a )
		{
			RebuildStreamOptions();

			// ---- game audio capture -------------------------------------
			// The switch is audio.capture (per profile, default on), applied live
			// by main.cpp's ritz_apply_config_live() -> Capture::SetWanted(); the
			// row id is the config key itself. While capture is Active the Game
			// volume row acts on the capture sink (no guessing); it needs no
			// wpctl, so it is drawn whatever the wpctl state below is.
			a.Group( "Game audio" );

			a.Switch( "audio.capture", "Capture game audio",
				ui::AnyBind::Of<bool>(
					[]{ EnsureConfigLoaded(); return s_CachedSettings.audio.capture; },
					[]( bool b )
					{
						EnsureConfigLoaded();
						s_CachedSettings.audio.capture = b;
						QueueSave();
					} ) )
				.Help( "Plays the game's audio into a private output of its own first, so the Game "
				       "volume below controls exactly the game's sound and nothing is guessed. The "
				       "processed audio then goes wherever the game's audio was headed before it was "
				       "captured (for example EasyEffects, when it processes all outputs), otherwise "
				       "to the system's default output. Turning it off sends the game's audio where it "
				       "went before. If the private output cannot be created, the Mixer falls back to "
				       "guessing the game's stream; Diagnostics says why." )
				.Default( true )
				.Keywords( "capture game audio sink pipewire output route easyeffects default" );

			if ( s_bCaptureActive )
			{
				EmitGameVolumeRow( a, "Game volume",
					ui::AnyBind::Of<int>(
						[]{
							return (int)std::lround(
								ResolveDisplayVolume( s_PendingCapture, Audio::Capture::GetVolume() ) * 100.0f );
						},
						[]( int nPercent )
						{
							const float flFrac = std::clamp( nPercent, 0, 150 ) / 100.0f;
							Audio::Capture::SetVolume( flFrac );
							// Saved per GAME (games.<key>.game_volume), not per profile:
							// only a slider move persists, never an external change
							// mirrored into GetVolume(), and never mute. The 5% step
							// keeps a drag to a few (coalesced) writes. No key = not saved.
							if ( const std::optional<std::string> oKey = config::SessionGameVolumeKey() )
								config::SetGameVolume( *oKey, flFrac );
							s_PendingCapture.bHasVolume = true;
							s_PendingCapture.flVolume   = flFrac;
							s_PendingCapture.tWhen      = std::chrono::steady_clock::now();
						} ),
					ui::AnyBind::Of<bool>(
						[]{ return ResolveDisplayMute( s_PendingCapture, Audio::Capture::GetMute() ); },
						[]( bool bMuted )
						{
							Audio::Capture::SetMute( bMuted );
							s_PendingCapture.bHasMute = true;
							s_PendingCapture.bMuted   = bMuted;
							s_PendingCapture.tWhen    = std::chrono::steady_clock::now();
						} ),
					"The game's volume, applied to the private output all of its sound plays through "
					"(Capture game audio, above), so it moves every one of the game's sounds together "
					"and cannot land on the wrong app. Going above 100% is a real boost that can "
					"distort the sound, not just extra headroom. The volume is saved per game; mute is not saved." );
			}

			// ---- Dynamics ---------------------------------------------
			// The compressor/limiter on the captured audio (Audio/Dynamics.h). It
			// only has something to process while capture is active, so the rows are
			// dimmed with the reason otherwise; the choice is still saved per profile.
			a.Group( "Dynamics" );

			a.Choice( "audio.dynamics.preset", "Preset",
				ui::AnyBind::Of<int>( []{ return DynamicsPresetIndex(); }, []( int n ) { SetDynamicsPreset( n ); } ),
				kDynamicsPresetOptions, std::size( kDynamicsPresetOptions ) )
				.Help( "Evens out the game's sound. Even out brings quiet footsteps up next to loud "
				       "gunfire without pumping. Night squeezes the range hard so nothing is loud "
				       "enough to wake anyone. Limiter only leaves the sound alone and just stops "
				       "peaks from clipping. Off leaves the sound untouched. Picking one fills in "
				       "the Advanced values below; changing any of them switches to Custom. Adds "
				       "up to 1.5 ms of delay while it is on, and none while it is off. Works on "
				       "the captured game audio only (Capture game audio, above)." )
				.Default( 0 )
				.Keywords( "dynamics compressor limiter even out night loudness footsteps normalize preset" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			a.Group( "Dynamics: Advanced" );

			a.Slider( "audio.dynamics.threshold_db", "Threshold", DynamicsBind( &config::AudioDynamicsSettings::threshold_db ) )
				.Help( "How loud a sound has to be before it gets turned down. Sounds quieter than this "
				       "are left as they are (apart from Make-up gain). Lower catches more of the sound." )
				.Range( -80.0f, 0.0f ).Step( 1.0f ).Unit( " dB" ).Default( -24.0f )
				.Keywords( "dynamics threshold compressor level db" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			a.Slider( "audio.dynamics.ratio", "Ratio", DynamicsBind( &config::AudioDynamicsSettings::ratio ) )
				.Help( "How hard sounds above the Threshold are turned down. 1 does nothing; 3 turns a "
				       "9 dB overshoot into 3 dB; 20 is close to a wall." )
				.Range( 1.0f, 20.0f ).Step( 0.5f ).Unit( ":1" ).Default( 3.0f )
				.Keywords( "dynamics ratio compressor squeeze" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			a.Slider( "audio.dynamics.attack_ms", "Attack", DynamicsBind( &config::AudioDynamicsSettings::attack_ms ) )
				.Help( "How fast the sound is turned down once it gets loud. Short catches sharp shots "
				       "but can sound choppy; long lets the first bang through." )
				.Range( 1.0f, 100.0f ).Step( 1.0f ).Unit( " ms" ).Default( 10.0f )
				.Keywords( "dynamics attack compressor speed" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			a.Slider( "audio.dynamics.release_ms", "Release", DynamicsBind( &config::AudioDynamicsSettings::release_ms ) )
				.Help( "How fast the sound comes back up after a loud moment. Short follows the game "
				       "closely but can pump; long is smoother but quiet sounds after a shot stay low "
				       "for longer." )
				.Range( 10.0f, 1000.0f ).Step( 10.0f ).Unit( " ms" ).Default( 150.0f )
				.Keywords( "dynamics release compressor recovery pumping" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			a.Slider( "audio.dynamics.makeup_db", "Make-up gain", DynamicsBind( &config::AudioDynamicsSettings::makeup_db ) )
				.Help( "Turns everything up after the loud parts were turned down, so quiet sounds "
				       "come out louder. The limiter still keeps the result under the Ceiling." )
				.Range( -24.0f, 24.0f ).Step( 1.0f ).Unit( " dB" ).Default( 6.0f )
				.Keywords( "dynamics makeup gain boost louder" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			a.Slider( "audio.dynamics.ceiling_db", "Ceiling", DynamicsBind( &config::AudioDynamicsSettings::ceiling_db ) )
				.Help( "The loudest level the sound is allowed to reach. Peaks above it are pulled down "
				       "a hair ahead of time instead of being clipped, which sounds like distortion. "
				       "-1 dB is a safe default." )
				.Range( -24.0f, 0.0f ).Step( 0.5f ).Unit( " dB" ).Default( -1.0f )
				.Keywords( "dynamics ceiling limiter clip peak max level" )
				.DisabledUnless( CaptureOn, "Capture game audio is off or not active" );

			// wpctl is a RUNTIME dependency (DECISIONS.md #22) of everything
			// below: the stream guessing (the fallback path) and the per-app
			// rows. Without it there is nothing more to control, so the area
			// builds only the capture rows above and the Diagnostics readout
			// rather than a sheet full of dead sliders.
			if ( s_AreaState.bWpctlAvailable )
			{
				a.Group( s_bCaptureActive ? "Game stream (used if capture is off)" : "Game stream" );

				a.Choice( "audio.stream", "Stream",
					ui::AnyBind::Of<int>(
						[]{ return CurrentPickerIndex(); },
						[]( int nIndex )
						{
							if ( nIndex < 0 || nIndex >= (int)s_vecOptionIdentity.size() )
								return;
							s_sManualNode = s_vecOptionIdentity[ nIndex ];
							PushManualSelectionToLiveState();
							SaveManualNode();
						} ),
					s_vecStreamOptions.data(), s_vecStreamOptions.size() )
					.Help( "Which audio stream is this game's, for when Capture game audio is off or "
					       "unavailable (while it is on, the game's sound is captured, not guessed, and "
					       "this has no effect on Game volume). Automatic finds it on its own, or pick "
					       "one by hand if the wrong app's audio gets controlled instead -- remembered "
					       "for this game across relaunches." )
					.Default( 0 )
					.Keywords( "stream node pipewire application pick manual override wpctl" );

				// The matched stream's own row, the FALLBACK path: present only
				// when capture is not active and detection actually resolved
				// something. It drives Audio::RequestVolume(), NOT the per-node
				// call -- when a game owns several nodes this row moves all of
				// them together, which is the behaviour the legacy primary row
				// had and the per-stream rows below cannot express.
				if ( !s_bCaptureActive && s_AreaState.bDetected )
				{
					const std::string sPrimaryLabel = PrimaryStreamRowLabel();

					EmitGameVolumeRow( a, sPrimaryLabel,
						ui::AnyBind::Of<int>(
							[]{
								return (int)std::lround(
									ResolveDisplayVolume( s_PendingPrimary, s_AreaState.flVolume ) * 100.0f );
							},
							[]( int nPercent )
							{
								const float flFrac = std::clamp( nPercent, 0, 150 ) / 100.0f;
								Audio::RequestVolume( flFrac );
								s_PendingPrimary.bHasVolume = true;
								s_PendingPrimary.flVolume   = flFrac;
								s_PendingPrimary.tWhen      = std::chrono::steady_clock::now();
							} ),
						ui::AnyBind::Of<bool>(
							[]{ return ResolveDisplayMute( s_PendingPrimary, s_AreaState.bMuted ); },
							[]( bool bMuted )
							{
								Audio::RequestMute( bMuted );
								s_PendingPrimary.bHasMute = true;
								s_PendingPrimary.bMuted   = bMuted;
								s_PendingPrimary.tWhen    = std::chrono::steady_clock::now();
							} ),
						"The game's volume, on the stream gamescope guessed is the game's (Capture game "
						"audio is off or unavailable). Going above 100% is a real boost that can distort "
						"the sound, not just extra headroom, and moves every one of the game's sounds "
						"together. While Automatic detection is picking the stream (the Stream row above "
						"reads \"Automatic\"), this row is titled with the detected stream's own name so "
						"you can confirm it found the right app; picking a stream by hand reverts the "
						"title to plain \"Game volume\"." );
				}

				// ---- one row per live stream ---------------------------
				// Issue #36. On the fallback path the matched stream is skipped
				// because the Game volume row above already covers it -- listing
				// it twice would put two controls on one node, which is how a
				// value fights itself. On the capture path Game volume is the
				// sink, not a stream, so every stream (the game's own included)
				// keeps its row. Our own capture nodes never get here: Volume.cpp
				// drops them at the source.
				a.Group( "Streams" );
				for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
				{
					if ( !s_bCaptureActive && s_AreaState.bDetected && IsPrimaryNode( c.nNodeId ) )
						continue;

					// The id carries the NODE, which is what makes this row
					// mean the same application from one rebuild to the
					// next. A positional id would not.
					const std::string sId = "audio.node." + std::to_string( c.nNodeId );
					const int nNodeId = c.nNodeId;

					a.Slider( sId.c_str(), StreamName( c ).c_str(), BindNodeVolume( nNodeId ) )
						.Help( "This app's own volume, separate from the others. Changing it never "
						       "affects any other app's sound." )
						.Range( 0.0f, 150.0f )
						.Step( 5.0f )    // 31 positions, as Game volume above
						.Default( 100 )
						.Unit( "%" )
						.Keywords( "volume stream application per-app mixer" )
						.Param( "mute", "Mute", BindNodeMute( nNodeId ) )
							.Help( "Mutes this app's sound without changing its volume." )
							.Default( false );
				}
			}

			// ---- Diagnostics ------------------------------------------
			// Every status line the legacy panel printed as prose lands
			// here as a .Live() fact. D13.5's precedent: a statement about
			// live state is a readout, not a setting, and a Facts row
			// cannot be given a control at all.
			a.Group( "Diagnostics" );
			a.Facts( "audio.server", "Audio server",
				[]{
					std::string s;
					if ( s_bCaptureActive )
						s = "capture on  ·  ";
					else if ( Audio::Capture::GetWanted() )
						s = "capture not active  ·  ";
					return s + DetectionSummary() + "  ·  " +
					       std::to_string( s_AreaState.nTotalAudioStreams ) + " stream" +
					       ( s_AreaState.nTotalAudioStreams == 1 ? "" : "s" );
				} )
				.Help( "Shows whether the game's audio is being captured, what Game volume is driving, "
				       "how the game's stream is being found when it is not captured, and how many sounds "
				       "are playing. Read-only, nothing here can be changed." )
				.Keywords( "capture sink wpctl pipewire wireplumber detection status diagnostics" )
				.Live( "capture", []{
					return ui::Fact{ "capture", CaptureStatusLine() };
				} )
				.Live( "plays_to", []{
					return ui::Fact{ "capture plays to", Audio::Capture::Active()
						? Audio::Capture::Route::Describe( Audio::Capture::PlaybackRoute() )
						: std::string( "nothing (capture is not active)" ) };
				} )
				.Live( "dynamics", []{
					return ui::Fact{ "dynamics", DynamicsStatusLine() };
				} )
				.Live( "drives", []{
					return ui::Fact{ "game volume drives", GameVolumeDrives() };
				} )
				.Live( "wpctl", []{
					return ui::Fact{ "wpctl", s_AreaState.bWpctlAvailable
						? "present"
						: "NOT FOUND -- install WirePlumber's CLI to control per-app volume" };
				} )
				.Live( "match", []{
					return ui::Fact{ "match", DetectionSummary() };
				} )
				.Live( "matched_nodes", []{
					std::string s;
					for ( int nId : s_AreaState.vecMatchedNodeIds )
						s += ( s.empty() ? "" : ", " ) + std::to_string( nId );
					return ui::Fact{ "matched nodes", s.empty() ? "none" : s };
				} )
				.Live( "streams", []{
					// The multi-candidate case the legacy panel warned
					// about: several streams tied at the winning tier and
					// the most recently created one won. Reported, never
					// silently resolved.
					std::string s = std::to_string( s_AreaState.nTotalAudioStreams ) + " total";
					if ( s_AreaState.nCandidatesAtWinningTier > 1 )
						s += "  ·  " + std::to_string( s_AreaState.nCandidatesAtWinningTier ) +
						     " candidates tied, using the most recent";
					return ui::Fact{ "streams", s };
				} )
				.Live( "override", []{
					const std::string &sManual = s_sManualNode;
					if ( sManual.empty() )
						return ui::Fact{ "manual override", "none -- detection is automatic" };
					return ui::Fact{ "manual override", sManual +
						( s_AreaState.bManualSelectionStale ? "  (not streaming right now)" : "" ) };
				} )
				.Live( "unpinnable", []{
					// The streams the picker could not offer, and why. This
					// is where the legacy combo's greyed rows and their
					// tooltip went.
					int nUnpinnable = 0;
					for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
						if ( CandidateIdentity( c ).empty() )
							++nUnpinnable;
					if ( !nUnpinnable )
						return ui::Fact{ "not pinnable", "none -- every stream reported a name" };
					return ui::Fact{ "not pinnable", std::to_string( nUnpinnable ) +
						" stream(s) reported no application name or binary, so they cannot be "
						"remembered across a relaunch and are not offered in Stream above" };
				} );
		}
	}

	void PanelAudio_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
		a.Keywords( "audio volume mute mixer pipewire wireplumber wpctl stream per-app" );
		a.Summary( []{
			if ( s_bCaptureActive )
			{
				const int nCaptured = Audio::Capture::StreamCount();
				return "game audio captured  ·  " + std::to_string( nCaptured ) +
				       ( nCaptured == 1 ? " stream" : " streams" );
			}
			if ( !s_AreaState.bWpctlAvailable )
				return std::string( "wpctl not found -- per-app volume unavailable" );
			const int nStreams = (int)s_vecAreaStreams.size();
			return std::to_string( nStreams ) + ( nStreams == 1 ? " stream" : " streams" ) +
			       "  ·  " + DetectionSummary();
		} );

		// The row set is discovered, not declared -- see the note above
		// BuildAudioArea and ui::Area::Rebuilds().
		a.Rebuilds( AudioGeneration, BuildAudioArea );
	}
}
