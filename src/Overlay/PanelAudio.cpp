// M5 Audio panel -- see PanelAudio.h and superdoc/planning/SPEC.md's
// Feature 5 ("PipeWire volume").
//
// This panel is deliberately thin: Audio::GetState() already hands back a
// fully-resolved snapshot (wpctl availability, detection method, matched/
// candidate counts, current display-fraction volume, mute) and the curve is
// applied inside src/Audio/Volume.cpp -- see DisplayToLinearVolume()'s
// comment there. The slider below reads/writes display-fraction units only
// and never re-applies a curve of its own (that would double-apply it).
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

#include "../Audio/Volume.h"
#include "Config/ConfigManager.h"
#include "UI/Registry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

		void PushManualSelectionToLiveState()
		{
			const std::string &sBinary = s_CachedSettings.audio.manual_node_binary;
			Audio::SetManualSelection( sBinary.empty() ? std::nullopt : std::optional<std::string>( sBinary ) );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;

			s_CachedSettings = config::ResolveEffective( config::SessionAppId() );
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
			Mix( s_AreaState.bDetected ? "detected" : "undetected" );
			Mix( s_AreaState.sManualSelection );
			Mix( s_CachedSettings.audio.manual_node_binary );
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
			const std::string &sManual = s_CachedSettings.audio.manual_node_binary;
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
			const std::string &sManual = s_CachedSettings.audio.manual_node_binary;
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

		// ---- the builder -----------------------------------------------
		void BuildAudioArea( ui::Area &a )
		{
			RebuildStreamOptions();

			// wpctl is a RUNTIME dependency (DECISIONS.md #22). Without it
			// there is nothing to control, so the area builds exactly one
			// row -- the Diagnostics readout -- rather than a sheet full of
			// dead sliders. The Overview and that row say why.
			if ( s_AreaState.bWpctlAvailable )
			{
				a.Group( "Game stream" );

				a.Choice( "audio.stream", "Stream",
					ui::AnyBind::Of<int>(
						[]{ return CurrentPickerIndex(); },
						[]( int nIndex )
						{
							if ( nIndex < 0 || nIndex >= (int)s_vecOptionIdentity.size() )
								return;
							s_CachedSettings.audio.manual_node_binary = s_vecOptionIdentity[ nIndex ];
							PushManualSelectionToLiveState();
							QueueSave();
						} ),
					s_vecStreamOptions.data(), s_vecStreamOptions.size() )
					.Help( "Which PipeWire stream counts as this game's audio. Automatic matches by "
					       "process id, then by process name, then falls back to the newest stream "
					       "since launch. Picking one by hand pins it for this game and is remembered "
					       "across relaunches by application name, not by node id -- node ids are a "
					       "fresh number every session." )
					.Default( 0 )
					.Keywords( "stream node pipewire application pick manual override wpctl" );

				// The matched stream's own row, present only when detection
				// actually resolved something. It drives Audio::RequestVolume(),
				// NOT the per-node call -- when a game owns several nodes this
				// row moves all of them together, which is the behaviour the
				// legacy primary row had and the per-stream rows below cannot
				// express.
				if ( s_AreaState.bDetected )
				{
					a.Slider( "audio.volume", "Game volume",
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
							} ) )
						.Help( "Volume of the matched game stream. Above 100% is a real boost applied "
						       "by WirePlumber, not headroom -- it can clip. When the game owns several "
						       "streams this moves all of them together." )
						.Range( 0.0f, 150.0f )
						.Default( 100 )
						.Unit( "%" )
						.Keywords( "volume gain level loudness game audio" )
						.Param( "mute", "Mute",
							ui::AnyBind::Of<bool>(
								[]{ return ResolveDisplayMute( s_PendingPrimary, s_AreaState.bMuted ); },
								[]( bool bMuted )
								{
									Audio::RequestMute( bMuted );
									s_PendingPrimary.bHasMute = true;
									s_PendingPrimary.bMuted   = bMuted;
									s_PendingPrimary.tWhen    = std::chrono::steady_clock::now();
								} ) )
							.Help( "Silences the stream without changing its volume." )
							.Default( false );
				}

				// ---- one row per live stream ---------------------------
				// Issue #36. The matched stream is skipped because the row
				// above already covers it -- listing it twice would put two
				// controls on one node, which is how a value fights itself.
				a.Group( "Streams" );
				for ( const Audio::StreamCandidate &c : s_vecAreaStreams )
				{
					if ( s_AreaState.bDetected && IsPrimaryNode( c.nNodeId ) )
						continue;

					// The id carries the NODE, which is what makes this row
					// mean the same application from one rebuild to the
					// next. A positional id would not.
					const std::string sId = "audio.node." + std::to_string( c.nNodeId );
					const int nNodeId = c.nNodeId;

					a.Slider( sId.c_str(), StreamName( c ).c_str(), BindNodeVolume( nNodeId ) )
						.Help( "Volume of this application's audio stream, applied to its PipeWire "
						       "node directly. Independent of every other row here: this cannot move "
						       "another application's volume." )
						.Range( 0.0f, 150.0f )
						.Default( 100 )
						.Unit( "%" )
						.Keywords( "volume stream application per-app mixer" )
						.Param( "mute", "Mute", BindNodeMute( nNodeId ) )
							.Help( "Silences this stream without changing its volume." )
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
					return DetectionSummary() + "  ·  " +
					       std::to_string( s_AreaState.nTotalAudioStreams ) + " stream" +
					       ( s_AreaState.nTotalAudioStreams == 1 ? "" : "s" );
				} )
				.Help( "What the audio backend currently reports: whether wpctl is present, how the "
				       "game's stream was matched, and how many streams exist. Read-only -- every "
				       "value here is observed, not set." )
				.Keywords( "wpctl pipewire wireplumber detection status diagnostics" )
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
					const std::string &sManual = s_CachedSettings.audio.manual_node_binary;
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
