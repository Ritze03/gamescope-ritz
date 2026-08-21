// Milestone M5 -- the "Audio" panel of the settings overlay: a single volume
// fader and a mute toggle over the hosted game's PipeWire stream, plus honest
// status reporting when the stream can't be found or `wpctl` is missing. See
// superdoc/planning/SPEC.md's Feature 5 ("PipeWire volume") and Build order
// M5, and superdoc/planning/DECISIONS.md #22/#23.
//
// All of the actual volume logic (curve, PID matching, the wpctl shell-out
// and its background thread) lives in src/Audio/Volume.h -- this panel only
// ever reads Audio::GetState() and calls Audio::RequestVolume()/
// Audio::RequestMute(). It does not persist anything (DECISIONS.md: volume
// is intentionally not part of our config -- WirePlumber already restores
// per-application volume itself).
#pragma once

namespace gamescope
{
	// Draws the Audio panel's ImGui window. Must be called from the same
	// place/thread SettingsOverlay draws its own window (steamcompmgr
	// thread, between ImGui::NewFrame() and ImGui::Render()), matching
	// PanelDisplay_Draw()/PanelShaders_Draw() -- Audio::GetState() and
	// Audio::Request*() are internally synchronized (Volume.cpp's own
	// mutex/atomics) so this isn't a correctness requirement here the way
	// it is for the other panels, but keeping every panel draw on one
	// thread is the established pattern.
	void PanelAudio_Draw();
}
