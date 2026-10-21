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
//
// Since 2026-10-07 the "Game volume" row has two paths: while game audio capture
// is active (src/Audio/Capture.h) it controls the capture sink, and the stream
// guessing above is only the fallback for when capture is off or failed. The
// panel also carries the per-profile `audio.capture` switch and Capture's
// status in Diagnostics. See superdoc/features/audio-mixer.md.
#pragma once

#include <cstddef>
#include <string>

#include "Audio/Dynamics.h"
#include "Config/ConfigSchema.h"

namespace gamescope
{
	namespace ui { class Registry; class Area; }

	// The "Game volume" row's own title (audio.stream.volume in
	// BuildAudioArea(), PanelAudio.cpp). 2026-09-27, the user's own words:
	// "if the window doesnt have a fixed audio stream, show the audio
	// stream name instead of 'Game volume'. Just so the user can quickly
	// confirm, that it was detected correctly."
	//
	// A pure function of the three things the decision actually depends
	// on, so it is testable (tests/test_audio_volume.cpp) with no live
	// PipeWire state, no config, no ImGui and no Registry -- everything
	// PanelAudio.cpp's own PrimaryStreamRowLabel() threads this through
	// from Audio::VolumeState/s_sManualNode. Header-only/inline rather
	// than declared here and defined in PanelAudio.cpp, so a test needs
	// nothing else out of that translation unit (which pulls in ImGui,
	// ui::Registry and config::ConfigManager -- none of which this one
	// decision needs) and nothing added to tests/meson.build.
	//
	//   bManualStreamPicked  -- s_sManualNode is non-empty: the user
	//                           pinned a stream by hand and already knows
	//                           which one it is, so the title stays the
	//                           plain, stable "Game volume".
	//   sPrimaryStreamName   -- the detected stream's own name (same
	//                           application-name/media-name precedence
	//                           the Stream picker's own option text uses,
	//                           PanelAudio.cpp's StreamName()), or empty
	//                           when there is nothing usable to show (no
	//                           match, or a matched node reported neither
	//                           an application name nor a media name).
	//   nOtherMatchedNodes   -- how many OTHER nodes share the winning
	//                           identity and move with this one slider
	//                           (VolumeState::vecMatchedNodeIds.size() -
	//                           1, i.e. 0 for the common single-node
	//                           case). Appended as "(+N)" rather than
	//                           silently naming the row after only one of
	//                           several streams moving together.
	//
	// Falls back to "Game volume" whenever there is nothing better to
	// show. The row's id (audio.stream.volume) never changes -- only
	// this title does -- so the settings audit, the palette's identity
	// and any test keying on the id are all unaffected.
	inline std::string AudioMixerPrimaryRowLabel( bool bManualStreamPicked,
	                                               const std::string &sPrimaryStreamName,
	                                               size_t nOtherMatchedNodes )
	{
		if ( bManualStreamPicked || sPrimaryStreamName.empty() )
			return "Game volume";

		std::string sTitle = sPrimaryStreamName;
		if ( nOtherMatchedNodes > 0 )
			sTitle += "  (+" + std::to_string( nOtherMatchedNodes ) + ")";
		return sTitle;
	}

	// The Dynamics group's stored choice -> what the processor runs. `audio.dynamics.preset`
	// is "off" | "even_out" | "night" | "limiter_only" | "custom". A named preset runs the
	// library's own values (PresetParams), which the sliders were loaded with when it was
	// picked; "custom" runs the six stored values; anything else (an unknown string that
	// somehow got past the loader) is off. Used by main.cpp's ritz_apply_config_live() and
	// by the panel, so what you hear is never different from what the rows say.
	inline Audio::Dynamics::Params AudioDynamicsParams( const config::AudioDynamicsSettings &d )
	{
		using namespace Audio::Dynamics;
		if ( d.preset == "even_out" )
			return PresetParams( Preset::EvenOut );
		if ( d.preset == "night" )
			return PresetParams( Preset::Night );
		if ( d.preset == "limiter_only" )
			return PresetParams( Preset::LimiterOnly );
		Params p;
		if ( d.preset == "custom" )
		{
			p.bEnabled = true;
			p.flThresholdDb = d.threshold_db;
			p.flRatio = d.ratio;
			p.flAttackMs = d.attack_ms;
			p.flReleaseMs = d.release_ms;
			p.flMakeupDb = d.makeup_db;
			p.flCeilingDb = d.ceiling_db;
		}
		return p;
	}

	// P3 part B: the E2 registration for `audio.mixer`.
	//
	// This is the first DYNAMIC area in the product -- its row set is one
	// row per live PipeWire stream, and streams appear and disappear while
	// the overlay is open. See ui::Area::Rebuilds() in Overlay/UI/Registry.h
	// for how that is reconciled with a registry designed around startup
	// declaration, and AUTONOMOUS-DECISIONS.md D14 for why the alternative
	// (a fixed pool of positional slots) was rejected.
	void PanelAudio_RegisterArea( ui::Registry &reg );
}
