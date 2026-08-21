// PipeWire volume control for the game gamescope is hosting.
//
// v1 shells out to `wpctl` (WirePlumber's CLI) rather than talking to
// PipeWire directly - see superdoc/planning/pipewire-loudness.md and
// DECISIONS.md #22/#23 for why. All wpctl calls, plus the /proc walk used
// to find the game's audio node, happen on a dedicated background thread;
// nothing here ever blocks the caller on a subprocess spawn.
//
// Not persisted: WirePlumber already remembers per-application stream
// volume itself (node.stream.restore-props), so gamescope deliberately
// does not keep its own copy - see the spec's PipeWire volume section.
// This is a live control surface only.
#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace gamescope::Audio
{
	// A UI-facing snapshot of the last poll. Cheap to copy, safe to read
	// every frame - never touches wpctl itself.
	struct VolumeState
	{
		bool bWpctlAvailable = true;  // false once `wpctl` is confirmed missing from the system
		bool bDetected = false;       // an audio stream node was found for the target process tree
		int nMatchedNodes = 0;        // how many stream nodes matched (0, 1, or several)
		float flVolume = 1.0f;        // display-curve fraction, 0..1 (up to 1.5 with boost) - see DisplayToLinearVolume
		bool bMuted = false;
	};

	// ---- Volume curve ---------------------------------------------------
	// PipeWire's SPA_PROP_channelVolumes (and what `wpctl set-volume`/
	// `get-volume` read and write) is raw linear amplitude. UI sliders in
	// this ecosystem (pavucontrol, wpctl's own docs) instead present a
	// perceptual cubic curve: linear = display_fraction^3. This is the one
	// place that conversion happens - callers (UI code) should only ever
	// think in display fractions, never in raw linear amplitude.
	constexpr float DisplayToLinearVolume( float flDisplayFraction )
	{
		return flDisplayFraction * flDisplayFraction * flDisplayFraction;
	}

	inline float LinearToDisplayVolume( float flLinear )
	{
		return std::cbrt( flLinear );
	}

	// ---- Lifecycle --------------------------------------------------------

	// Starts the background polling thread. Safe to call more than once -
	// only the first call has any effect.
	void Init();

	// Sets (or clears, with 0) the root PID of the process tree to search
	// for an audio stream node - e.g. steamcompmgr's nPrimaryChildPid, the
	// PID gamescope's reaper wraps. Cheap and non-blocking.
	void SetTargetPid( pid_t nRootPid );

	// Non-blocking read of the last-known state. Safe to call every frame.
	VolumeState GetState();

	// Queues a volume/mute change to be applied on the background thread.
	// flDisplayFraction is in display-curve units (see above), 0..1.5.
	void RequestVolume( float flDisplayFraction );
	void RequestMute( bool bMuted );

	// ---- Parsing helpers ---------------------------------------------------
	// Pure functions with no subprocess/thread involvement, factored out so
	// they can be exercised directly by tests against captured real `wpctl`
	// output. Not part of the stable control-surface API above.
	namespace Parse
	{
		// Node id + display name for every entry directly under a "Streams:"
		// section of `wpctl status` output (both Audio and Video - callers
		// filter by media.class via an `inspect` call). Port sub-lines
		// (which contain " > ") are skipped.
		std::vector<std::pair<int, std::string>> StatusStreamNodes( std::string_view svStatusOutput );

		// Looks up a `key = "value"` line from `wpctl inspect` output.
		std::optional<std::string> InspectField( std::string_view svInspectOutput, std::string_view svKey );

		bool IsAudioOutputStream( std::string_view svInspectOutput );

		struct VolumeReading
		{
			float flLinear;
			bool bMuted;
		};
		// Parses `wpctl get-volume` output, e.g. "Volume: 0.30" or
		// "Volume: 0.30 [MUTED]".
		std::optional<VolumeReading> GetVolumeOutput( std::string_view svOutput );

		// Formats a linear volume for a wpctl command line without going
		// through a locale-sensitive float-to-string conversion (wpctl
		// expects a plain '.'-decimal number regardless of process locale).
		std::string FormatLinearVolume( float flLinear );
	}
}
