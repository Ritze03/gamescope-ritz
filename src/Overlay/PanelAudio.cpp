// M5 Audio panel -- see PanelAudio.h and superdoc/planning/SPEC.md's
// Feature 5 ("PipeWire volume").
//
// This panel is deliberately thin: Audio::GetState() already hands back a
// fully-resolved snapshot (wpctl availability, detection, matched node
// count, current display-fraction volume, mute) and the curve is applied
// inside src/Audio/Volume.cpp -- see DisplayToLinearVolume()'s comment
// there. The slider below reads/writes display-fraction units only and
// never re-applies a curve of its own (that would double-apply it).
//
// Two explicit "control isn't usable" states, both DECISIONS.md-mandated
// (#22/#23) rather than a silently-dead slider:
//   - wpctl missing entirely -> whole panel greyed, one status line.
//   - stream not detected for the target PID tree (common for sandboxed
//     Proton titles under pressure-vessel/bwrap, whose PID namespace
//     doesn't match what gamescope sees) -> slider/mute still drawn but
//     disabled, with a short explanation.
#include "PanelAudio.h"

#include "../Audio/Volume.h"
#include "Widgets.h"

#include <cmath>

#include "imgui.h"

namespace gamescope
{
	void PanelAudio_Draw()
	{
		const Audio::VolumeState state = Audio::GetState();

		// Placed clear of the M1 placeholder window and the M3/M6 panels so
		// all four are visible at once during manual testing.
		ImGui::SetNextWindowPos( ImVec2( 520.0f, 340.0f ), ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowSize( ImVec2( 380.0f, 180.0f ), ImGuiCond_FirstUseEver );

		ImGui::Begin( "Audio", nullptr, ImGuiWindowFlags_NoCollapse );

		if ( !state.bWpctlAvailable )
		{
			// DECISIONS.md #22: wpctl is a runtime-only dependency, not
			// build-time-checked -- surface its absence rather than a
			// control that silently does nothing.
			ImGui::TextColored( ImVec4( 0.95f, 0.35f, 0.35f, 1.0f ),
				"audio: wpctl not found" );
			ImGui::TextDisabled( "Install WirePlumber's CLI (wpctl) to control per-app volume." );
			ImGui::End();
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
		if ( ImGui::SliderInt( "Volume", &nUiPercent, 0, 150, "%d%%",
			ImGuiSliderFlags_AlwaysClamp ) )
		{
			Audio::RequestVolume( nUiPercent / 100.0f );
		}

		bool bMuted = state.bMuted;
		if ( widgets::Toggle( "Mute", &bMuted ) )
			Audio::RequestMute( bMuted );

		if ( bDisabled )
		{
			ImGui::EndDisabled();
			ImGui::Separator();
			// DECISIONS.md #23: an honestly-absent/disabled control beats a
			// slider that silently does nothing -- common for sandboxed
			// Proton titles (pressure-vessel/bwrap) whose PID namespace
			// doesn't match what gamescope sees.
			ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ),
				"audio: not detected" );
			ImGui::TextDisabled( "No matching PipeWire stream for this game yet -- "
				"some sandboxed Proton titles never match (known v1 gap)." );
		}
		else
		{
			ImGui::Separator();
			ImGui::TextDisabled( "audio: %d stream%s detected", state.nMatchedNodes,
				state.nMatchedNodes == 1 ? "" : "s" );
		}

		ImGui::End();
	}
}
