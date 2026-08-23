// Milestone M5 -- the "Audio" panel of the settings overlay: a single volume
// fader and a mute toggle over the hosted game's PipeWire stream, a manual
// stream picker, and honest status reporting for every way detection can
// come up short (wpctl missing, nothing matched, several candidates matched,
// a manual pick that isn't currently live). See superdoc/planning/SPEC.md's
// Feature 5 ("PipeWire volume") and Build order M5, and
// superdoc/planning/DECISIONS.md #22/#23.
//
// All of the actual volume/detection logic (curve, PID/name/recency
// matching, the wpctl shell-out and its background thread) lives in
// src/Audio/Volume.h -- this panel only ever reads Audio::GetState()/
// Audio::GetAvailableStreams() and calls Audio::RequestVolume()/
// Audio::RequestMute()/Audio::SetManualSelection(). It does not persist
// volume itself (DECISIONS.md: WirePlumber already restores per-application
// volume) -- but it does persist the manual stream *selection*, per game,
// via gamescope::config::AudioSettings::manual_node_binary (a node
// selection is not a volume value -- see Volume.h's header comment).
#pragma once

namespace gamescope
{
	// Draws the Audio panel's ImGui window. Must be called from the same
	// place/thread SettingsOverlay draws its own window (steamcompmgr
	// thread, between ImGui::NewFrame() and ImGui::Render()), matching
	// PanelDisplay_Draw()/PanelShaders_Draw() -- Audio::GetState() and
	// Audio::Request*()/SetManualSelection() are internally synchronized
	// (Volume.cpp's own mutex/atomics) so this isn't a correctness
	// requirement here the way it is for the other panels, but keeping
	// every panel draw on one thread is the established pattern.
	void PanelAudio_Draw();

	// E2 MIGRATION SEAM (P2), temporary -- see PanelDisplay.h's
	// PanelDisplay_DrawBody() for the full note. The same body with no
	// window around it, for ui::Area::Escape() to host in the E2 sheet.
	void PanelAudio_DrawBody();
}
