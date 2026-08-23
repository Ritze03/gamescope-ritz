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
	namespace ui { class Registry; class Area; }

	// P3 part B: the E2 registration for `audio.mixer`, replacing the
	// Escape() hatch below.
	//
	// This is the first DYNAMIC area in the product -- its row set is one
	// row per live PipeWire stream, and streams appear and disappear while
	// the overlay is open. See ui::Area::Rebuilds() in Overlay/UI/Registry.h
	// for how that is reconciled with a registry designed around startup
	// declaration, and AUTONOMOUS-DECISIONS.md D14 for why the alternative
	// (a fixed pool of positional slots) was rejected.
	void PanelAudio_RegisterArea( ui::Registry &reg );
	// Draws the Audio panel's ImGui window. Must be called from the same
	// place/thread SettingsOverlay draws its own window (steamcompmgr
	// thread, between ImGui::NewFrame() and ImGui::Render()), matching
	// PanelDisplay_Draw()/PanelShaders_Draw() -- Audio::GetState() and
	// Audio::Request*()/SetManualSelection() are internally synchronized
	// (Volume.cpp's own mutex/atomics) so this isn't a correctness
	// requirement here the way it is for the other panels, but keeping
	// every panel draw on one thread is the established pattern.
	void PanelAudio_Draw();

	// E2 MIGRATION SEAM (P2), temporary -- see Overlay/UI/Registry.h's
	// Escape() comment for why the hatch exists and what deletes it. The
	// same body with no window around it, for ui::Area::Escape() to host in
	// the E2 sheet. (P3 part A migrated Display and Shaders off this seam;
	// this area is a later part of P3 and still uses it.)
	void PanelAudio_DrawBody();
}
