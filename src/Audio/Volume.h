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
// This is a live control surface only. A manual node *selection* (which
// stream is "the game") is a different thing from the volume *value* and
// IS worth persisting per-game (see SetManualSelection() below and
// gamescope::config::AudioSettings::manual_node_binary) - WirePlumber has
// no concept of "which stream did the user mean," so there's nothing else
// that could remember that choice.
//
// ---- Detection strategies (tried in this order, most confident first) ----
// PID matching alone silently misses most real Steam/Proton games: Proton
// titles commonly run under pressure-vessel/bwrap sandboxing, which can put
// the real game process in a PID namespace gamescope's own /proc walk never
// sees. Rather than hiding the control in that case (the old v1 behavior),
// several strategies are tried and the result is always explained, never
// just silently absent:
//   1. Manual override  - an explicit per-game user choice (see below)
//      always wins outright and never falls back to a guess.
//   2. PID-tree match   - application.process.id against gamescope's own
//      descendant-PID walk (Process::GetChildPids). Exact when it works.
//   3. Process-name match - application.process.binary / application.name
//      against a descendant's own /proc/<pid>/comm. Covers the common
//      sandboxed case: the PID doesn't line up, but the process's own
//      self-reported name does. Both properties were confirmed present on
//      real Stream/Output/Audio nodes (see pipewire-loudness.md's §2 -
//      verified against live `wpctl inspect` output on a real system, not
//      assumed) - though clients populate them voluntarily, so this isn't
//      universal either.
//   4. "Newest stream since launch" - any Stream/Output/Audio node whose
//      object.serial (PipeWire's own never-reused monotonic counter,
//      confirmed present on every node via live `wpctl inspect`) is newer
//      than the baseline captured when the target PID was set. Strong
//      specifically because the common real case is "the hosted game is
//      the only new audio stream" - a last-resort heuristic, not a
//      guarantee.
// Whenever a tier finds more than one distinct candidate, the most
// recently created one is preferred, and the ambiguity is exposed (not
// hidden) via VolumeState::nCandidatesAtWinningTier so the panel can say
// so and point at the manual picker.
#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace gamescope::Audio
{
	// Which strategy (if any) produced the currently-controlled node(s).
	// See this header's top comment for what each one means.
	enum class DetectionMethod
	{
		NotDetected,  // nothing matched - see VolumeState::nTotalAudioStreams to tell "nothing is playing anywhere" from "something is, just not this game"
		Pid,          // application.process.id found in gamescope's own descendant-PID walk
		ProcessName,  // application.process.binary/application.name matched a descendant's own /proc comm
		Newest,       // heuristic: the newest Stream/Output/Audio node since the target PID was set
		Manual,       // user-picked via SetManualSelection()/the Audio panel's picker
	};

	// One Stream/Output/Audio node as `wpctl status`+`inspect` currently
	// report it - the picker's raw material (GetAvailableStreams()) and
	// also what SelectCandidate() (Volume.h's Parse namespace) scores.
	// Also what the Audio panel now binds a per-stream slider row to
	// directly (flVolume/bMuted below), one row per candidate, so every
	// active stream gets a live, independently-controllable control, not
	// just whichever one detection resolved as "the game."
	struct StreamCandidate
	{
		int nNodeId = 0;
		std::string sLabel;    // the name wpctl status prints (usually == application.name)
		std::string sBinary;   // application.process.binary, empty if the client never set it
		std::string sAppName;  // application.name, empty if the client never set it
		pid_t nPid = 0;        // application.process.id, 0 if absent/unparsable
		float flVolume = 1.0f; // this node's own live display-curve volume, 0..1.5 - see DisplayToLinearVolume
		bool bMuted = false;   // this node's own live mute state
	};

	// A UI-facing snapshot of the last poll. Cheap to copy, safe to read
	// every frame - never touches wpctl itself.
	struct VolumeState
	{
		bool bWpctlAvailable = true;  // false once `wpctl` is confirmed missing from the system
		bool bDetected = false;       // a node is currently being controlled (see eMethod for how it was found)
		DetectionMethod eMethod = DetectionMethod::NotDetected;
		int nMatchedNodes = 0;        // node(s) actually being controlled this poll (0 when not detected)
		std::vector<int> vecMatchedNodeIds; // the actual node id(s) behind nMatchedNodes - lets the panel skip re-listing the primary stream in the "every other active stream" section below it
		int nCandidatesAtWinningTier = 0; // how many distinct candidates tied at the winning strategy before the most-recent one was preferred (1 == unambiguous; >1 means the manual picker may be worth a look)
		int nTotalAudioStreams = 0;   // every Stream/Output/Audio node currently on the system, matched or not - lets the panel say "nothing is playing anywhere yet" vs "N streams exist, none are this game"
		bool bManualSelectionStale = false; // a manual override is set but didn't match any current stream (game not outputting audio right now, or the pick no longer applies)
		std::string sManualSelection; // echoes the currently-applied manual override identity (empty == none) - what SetManualSelection() last set, as actually picked up by the background thread
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
	// PID gamescope's reaper wraps. Cheap and non-blocking. Also resets the
	// "newest stream since launch" baseline (detection strategy 4 above) to
	// whatever streams already exist at that moment, so only streams that
	// appear afterward can ever count as "new."
	void SetTargetPid( pid_t nRootPid );

	// Non-blocking read of the last-known state. Safe to call every frame.
	VolumeState GetState();

	// Non-blocking read of every Stream/Output/Audio node currently known
	// (matched or not) - the manual picker's list. Safe to call every
	// frame; cheap to copy.
	std::vector<StreamCandidate> GetAvailableStreams();

	// Sets (or, with std::nullopt/empty, clears) a manual override: which
	// stream identity ("application.process.binary", falling back to
	// application.name) to always control, bypassing every automatic
	// strategy above. Once set, detection never silently falls back to a
	// guess if the chosen stream isn't currently present - see
	// VolumeState::bManualSelectionStale. Cheap and non-blocking; the
	// caller (PanelAudio) is responsible for persisting the choice (see
	// gamescope::config::AudioSettings::manual_node_binary) and calling
	// this again after a config (re)load.
	void SetManualSelection( std::optional<std::string> sBinaryOrAppName );

	// Queues a volume/mute change to be applied on the background thread,
	// targeting whichever node(s) the *current* detection/manual-override
	// result resolves as "the game" at apply time (VolumeState::
	// vecMatchedNodeIds) - i.e. the panel's primary/first slider.
	// flDisplayFraction is in display-curve units (see above), 0..1.5.
	void RequestVolume( float flDisplayFraction );
	void RequestMute( bool bMuted );

	// Same as above, but targets one specific node id directly, regardless
	// of what detection currently resolves - for the Audio panel's "every
	// other active stream" rows, each of which controls its own node and
	// is otherwise unrelated to game detection.
	void RequestVolumeForNode( int nNodeId, float flDisplayFraction );
	void RequestMuteForNode( int nNodeId, bool bMuted );

	// ---- Parsing + candidate-selection helpers -----------------------------
	// Pure functions with no subprocess/thread/filesystem involvement,
	// factored out so they can be exercised directly by tests against
	// captured real `wpctl` output (and synthetic NodeInfo lists for the
	// selection logic). Not part of the stable control-surface API above.
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

		// ---- Candidate selection (detection strategies 2-4 above) --------

		// One Stream/Output/Audio node's identity, as extracted from a
		// `wpctl inspect` call - built by the caller (all real I/O lives in
		// Volume.cpp) so SelectCandidate() below stays pure.
		struct NodeInfo
		{
			int nNodeId = 0;
			std::string sBinary;             // application.process.binary, empty if absent
			std::string sAppName;            // application.name, empty if absent
			std::optional<pid_t> onPid;      // application.process.id, parsed
			std::optional<long> olSerial;    // object.serial, parsed - PipeWire's own never-reused monotonic counter
		};

		struct SelectionResult
		{
			DetectionMethod eMethod = DetectionMethod::NotDetected;
			std::optional<int> onSelectedNodeId;   // which node to read the displayed volume from
			std::vector<int> vecMatchedNodeIds;    // every node id sharing the winning identity - commands are applied to all of these, by literal node id (not --pid, which is exactly what a PID-namespace mismatch breaks)
			int nCandidatesAtWinningTier = 0;      // how many distinct candidates tied at the winning tier before the most-recent one was preferred
		};

		// Chooses which of `nodes` (every Stream/Output/Audio node seen this
		// poll) is "the" game's audio, trying strategies in the order this
		// header's top comment describes. `vecDescendantPids`/
		// `vecDescendantNames` are gamescope's own /proc walk of the target
		// process tree (pids, and each one's /proc/<pid>/comm).
		// `olBaselineSerial` is the object.serial baseline captured when the
		// target PID was set (nullopt if not yet established - the "newest
		// since launch" tier is skipped in that case, since nothing is
		// distinguishable from "always existed" yet). `osManualBinary` is
		// SetManualSelection()'s current value.
		SelectionResult SelectCandidate(
			const std::vector<NodeInfo> &nodes,
			const std::vector<pid_t> &vecDescendantPids,
			const std::vector<std::string> &vecDescendantNames,
			std::optional<long> olBaselineSerial,
			const std::optional<std::string> &osManualBinary );
	}
}
