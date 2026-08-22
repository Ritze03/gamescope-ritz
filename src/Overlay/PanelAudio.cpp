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
#include "Widgets.h"
#include "Chrome.h"

#include <cmath>

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

		int s_nSelectedStream = -1; // index into the last-drawn GetAvailableStreams() list, picker-local only

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

		// "334 - paplay (pacat) [pid 3922017]", trimmed to whatever fields
		// this particular stream actually reported -- clients populate
		// application.* voluntarily (pipewire-loudness.md §2), so several
		// of these can be empty.
		std::string CandidateLabel( const Audio::StreamCandidate &candidate )
		{
			std::string sLabel = std::to_string( candidate.nNodeId ) + " - " +
				( candidate.sLabel.empty() ? "(unnamed stream)" : candidate.sLabel );
			if ( !candidate.sBinary.empty() && candidate.sBinary != candidate.sLabel )
				sLabel += " (" + candidate.sBinary + ")";
			if ( candidate.nPid != 0 )
				sLabel += " [pid " + std::to_string( candidate.nPid ) + "]";
			return sLabel;
		}

		// The identity SelectCandidate()/SetManualSelection() key on --
		// application.process.binary, falling back to application.name.
		// Mirrors Volume.cpp's Parse::IdentityKey() exactly (kept in sync
		// by hand since that helper is file-local there).
		std::string CandidateIdentity( const Audio::StreamCandidate &candidate )
		{
			return !candidate.sBinary.empty() ? candidate.sBinary : candidate.sAppName;
		}

		void DrawManualPicker( const Audio::VolumeState &state )
		{
			ImGui::TextUnformatted( "Pick a stream manually" );
			ImGui::TextDisabled( "Overrides automatic detection for this game -- remembered until cleared." );

			std::vector<Audio::StreamCandidate> vecStreams = Audio::GetAvailableStreams();

			if ( vecStreams.empty() )
			{
				ImGui::TextDisabled( "No PipeWire audio streams found on the system right now." );
			}
			else
			{
				if ( s_nSelectedStream >= (int)vecStreams.size() )
					s_nSelectedStream = -1;

				const char *pszPreview = ( s_nSelectedStream >= 0 )
					? vecStreams[ s_nSelectedStream ].sLabel.c_str() : "";
				if ( ImGui::BeginCombo( "##AudioStreamPicker", pszPreview ) )
				{
					for ( int i = 0; i < (int)vecStreams.size(); i++ )
					{
						const bool bSelectable = !CandidateIdentity( vecStreams[ i ] ).empty();
						const bool bSelected = ( i == s_nSelectedStream );

						ImGui::BeginDisabled( !bSelectable );
						std::string sItemLabel = CandidateLabel( vecStreams[ i ] );
						if ( ImGui::Selectable( sItemLabel.c_str(), bSelected ) )
							s_nSelectedStream = i;
						ImGui::EndDisabled();
						if ( !bSelectable && ImGui::IsItemHovered() )
							ImGui::SetTooltip( "This stream didn't report an application name/binary -- can't be remembered by identity." );
					}
					ImGui::EndCombo();
				}

				ImGui::SameLine();
				ImGui::BeginDisabled( s_nSelectedStream < 0 || s_nSelectedStream >= (int)vecStreams.size() );
				if ( ImGui::Button( "Use this stream" ) )
				{
					s_CachedSettings.audio.manual_node_binary = CandidateIdentity( vecStreams[ s_nSelectedStream ] );
					PushManualSelectionToLiveState();
					QueueSave();
				}
				ImGui::EndDisabled();
			}

			ImGui::BeginDisabled( s_CachedSettings.audio.manual_node_binary.empty() );
			if ( ImGui::Button( "Clear manual override" ) )
			{
				s_CachedSettings.audio.manual_node_binary.clear();
				PushManualSelectionToLiveState();
				QueueSave();
				s_nSelectedStream = -1;
			}
			ImGui::EndDisabled();

			if ( !s_CachedSettings.audio.manual_node_binary.empty() )
			{
				ImGui::TextDisabled( "Current override: %s%s", s_CachedSettings.audio.manual_node_binary.c_str(),
					state.bManualSelectionStale ? " (not currently streaming audio)" : "" );
			}
		}
	}

	void PanelAudio_Draw()
	{
		EnsureConfigLoaded();

		const Audio::VolumeState state = Audio::GetState();

		// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow(),
		// see Overlay/Chrome.h -- position/size unchanged from M5.
		if ( !chrome::BeginPanelWindow( "AUDIO", chrome::PanelId::Audio,
			ImVec2( 520.0f, 340.0f ), ImVec2( 380.0f, 180.0f ) ) )
			return;

		if ( !state.bWpctlAvailable )
		{
			// DECISIONS.md #22: wpctl is a runtime-only dependency, not
			// build-time-checked -- surface its absence rather than a
			// control that silently does nothing.
			ImGui::TextColored( ImVec4( 0.95f, 0.35f, 0.35f, 1.0f ),
				"audio: wpctl not found" );
			ImGui::TextDisabled( "Install WirePlumber's CLI (wpctl) to control per-app volume." );
			chrome::EndPanelWindow();
			return;
		}

		const bool bDisabled = !state.bDetected;

		if ( bDisabled )
			ImGui::BeginDisabled();

		// UI works in whole percent (0..150, matching the optional 150%
		// boost from SPEC.md's Audio panel row); Audio:: itself wants the
		// 0..1.5 display-fraction, so convert only at this edge -- no
		// curve is applied here, that already happened inside Volume.cpp.
		int nUiPercent = (int)std::lround( state.flVolume * 100.0f );
		// widgets::SliderInt draws the numeric readout in Mono/accent per the
		// design guide's numerals-are-always-Mono rule -- see Widgets.h.
		if ( widgets::SliderInt( "Volume", &nUiPercent, 0, 150, "%d%%",
			ImGuiSliderFlags_AlwaysClamp ) )
		{
			Audio::RequestVolume( nUiPercent / 100.0f );
		}

		bool bMuted = state.bMuted;
		if ( widgets::Toggle( "Mute", &bMuted ) )
			Audio::RequestMute( bMuted );

		if ( bDisabled )
			ImGui::EndDisabled();

		ImGui::Separator();

		if ( state.bDetected )
		{
			ImGui::TextColored( ImVec4( 0.45f, 0.85f, 0.45f, 1.0f ),
				"audio: %s (%d stream%s)", DetectionMethodLabel( state.eMethod ),
				state.nMatchedNodes, state.nMatchedNodes == 1 ? "" : "s" );
			if ( state.nCandidatesAtWinningTier > 1 )
			{
				ImGui::TextDisabled( "%d candidates matched -- using the most recently created. Pick a different one below if this is wrong.",
					state.nCandidatesAtWinningTier );
			}
		}
		else
		{
			// Never just "not detected" -- always say which of the honest
			// reasons applies (DECISIONS.md #23's spirit extended past the
			// PID-only v1: a missing dependency, a sandboxed PID, or simply
			// no audio anywhere yet all look different to the user).
			ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ), "audio: not detected" );
			if ( state.bManualSelectionStale )
			{
				ImGui::TextDisabled( "Manual selection \"%s\" isn't currently streaming audio -- "
					"waiting for it, or pick a different stream below.", state.sManualSelection.c_str() );
			}
			else if ( state.nTotalAudioStreams == 0 )
			{
				ImGui::TextDisabled( "No PipeWire audio streams exist on the system yet -- "
					"the game may not have started making sound." );
			}
			else
			{
				ImGui::TextDisabled( "%d audio stream%s on the system, but none matched this game "
					"(PID and process-name heuristics both came up empty -- common for sandboxed "
					"Proton titles). Pick the right one below.",
					state.nTotalAudioStreams, state.nTotalAudioStreams == 1 ? "" : "s" );
			}
		}

		ImGui::Separator();
		DrawManualPicker( state );

		chrome::EndPanelWindow();
	}
}
