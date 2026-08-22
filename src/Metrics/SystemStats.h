// Issue #28 (System Monitor part 2/3): CPU/RAM, AMD GPU, and media-playback
// metric collection for the CPU/GPU/Media modules FpsDisplay.cpp draws.
//
// Shape mirrors src/Audio/Volume.h exactly (see that header's own top
// comment) -- a detached background thread does all the blocking work
// (sysfs/proc reads, `playerctl` subprocess spawns), and the render thread
// only ever copies a small mutex-guarded snapshot out. Nothing declared
// here may be called from a context that isn't allowed to block; every
// Get*State() below is a cheap, non-blocking copy safe to call every frame.
//
// ---- Data sources (superdoc/planning/feature-ideas-research.md §3/§4,
// verified live on the scouting machine's AMD RX 7900 XTX / RADV) --------
//   CPU load : /proc/loadavg (1/5/15 min averages -- one read, no delta
//              math, per the research doc's own recommendation over
//              /proc/stat's per-core-jiffies-plus-delta approach).
//   RAM      : /proc/meminfo (MemTotal, MemAvailable).
//   GPU      : amdgpu sysfs -- gpu_busy_percent, mem_info_vram_used/total
//              under /sys/class/drm/cardN/device/, plus that card's hwmon
//              node (tempN_input, power1_average) under
//              /sys/class/hwmon/hwmonM/. Both cardN and hwmonM are resolved
//              at runtime by scanning, never hardcoded (the exact indices
//              are not portable across machines/reboots). This machine is
//              AMD -- on any system with no amdgpu sysfs node (NVIDIA,
//              Intel, or no discrete GPU at all) the scan simply finds
//              nothing and GpuState::bGpuFound stays false; the GPU module
//              reports itself as unavailable rather than guessing at a
//              vendor-neutral path that doesn't exist, or crashing on a
//              missing file.
//   Media    : `playerctl` subprocess, matching Audio/Volume.cpp's own
//              `wpctl`-shell-out precedent -- see that file's header
//              comment (DECISIONS.md #22/#23) for why shelling out beats
//              linking libdbus/sdbus-cpp here (no such dependency exists
//              in this build). "No player running" (playerctl missing, or
//              erroring/empty because nothing is currently open) is an
//              honestly-empty MediaState, not an error.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gamescope::Metrics
{
	// ---- CPU / RAM (polled every tick, see SystemStats.cpp's
	// kPollIntervalMs -- cheap plain-text /proc reads, fine at 1-2Hz). ------
	struct CpuState
	{
		bool bLoadAvailable = false;   // /proc/loadavg was readable this poll
		float flLoad1 = 0.0f;
		float flLoad5 = 0.0f;
		float flLoad15 = 0.0f;

		bool bMemAvailable = false;    // /proc/meminfo was readable this poll
		uint64_t ulMemUsedKb = 0;
		uint64_t ulMemTotalKb = 0;
	};

	// ---- AMD GPU (polled every tick; hwmon/drm paths resolved once at
	// thread start -- see SystemStats.cpp's ResolveGpuPaths()). ------------
	struct GpuState
	{
		bool bGpuFound = false;        // an amdgpu DRM device (gpu_busy_percent) was found at all -- false on non-AMD/no-GPU systems
		int nBusyPercent = 0;          // 0-100, only meaningful if bGpuFound
		uint64_t ulVramUsedBytes = 0;
		uint64_t ulVramTotalBytes = 0;

		bool bHwmonFound = false;      // this GPU's amdgpu hwmon node was found -- independent of bGpuFound (DRM and hwmon are separate sysfs trees, resolved separately)
		float flTempC = 0.0f;          // edge temp if labeled, else the hwmon node's first tempN_input
		float flPowerWatts = 0.0f;
	};

	enum class PlaybackStatus
	{
		Unknown, // no player, or status not yet polled
		Playing,
		Paused,
		Stopped,
	};

	// ---- Media playback (polled on its own slower cadence -- see
	// kMediaPollDivisor -- metadata changes rarely and this is the one
	// source that forks a subprocess). --------------------------------------
	struct MediaState
	{
		bool bPlayerAvailable = false; // a playerctl-visible MPRIS player is currently open
		PlaybackStatus eStatus = PlaybackStatus::Unknown;
		std::string sTitle;
		std::string sArtist;
	};

	// Starts the background poll thread. Safe to call more than once --
	// only the first call has any effect. Cheap and non-blocking (spawns
	// the thread and returns immediately; the thread itself does the actual
	// first read).
	void Init();

	// Non-blocking reads of the last-known snapshot. Safe to call every
	// frame -- never touches /proc, /sys, or playerctl itself.
	CpuState GetCpuState();
	GpuState GetGpuState();
	MediaState GetMediaState();

	// ---- 60-second history for the System Monitor's Statistics tab
	// (issue #40) ------------------------------------------------------------
	// Sampled at this poll thread's own native cadence (kPollIntervalMs in
	// SystemStats.cpp, 2Hz) -- 60s of history is only
	// kStatsHistoryCapacity samples at that rate, so no downsampling is
	// needed the way FpsDisplay.cpp's per-frame 240-sample buffer would
	// need if grown to cover 60s (that buffer stays exactly as-is; see its
	// own comment for why 60s is 15-60x longer than it was ever sized
	// for). FPS is read directly off g_ulLastAppFrametimeNs (src/commit.cpp)
	// on this same 2Hz cadence -- an independent, coarser sample of the
	// same underlying per-commit value FpsDisplay.cpp's own buffer tracks,
	// not derived from that buffer.
	inline constexpr int kStatsHistorySampleIntervalMs = 500; // == kPollIntervalMs (SystemStats.cpp)
	inline constexpr int kStatsHistoryCapacity = 120;         // 120 * 500ms = 60s

	struct HistorySample
	{
		float flCpuLoad1 = 0.0f;
		int   nGpuBusyPercent = 0;   // only meaningful if the snapshot's bGpuFound is true
		float flGpuTempC = 0.0f;     // only meaningful if the snapshot's bHwmonFound is true
		float flGpuPowerWatts = 0.0f; // only meaningful if the snapshot's bHwmonFound is true
		float flFps = 0.0f;          // 0 if no game frame had committed yet at sample time
	};

	struct HistorySnapshot
	{
		// Oldest-to-newest, holding only the samples actually collected
		// since collection last (re)started -- fewer than
		// kStatsHistoryCapacity entries during warm-up. Never stretched or
		// reinterpreted to fill the window; the caller is expected to plot
		// exactly this many samples and leave the rest of a 60s-wide axis
		// visibly blank (see the task's own "do not stretch a handful of
		// samples across the full width" requirement).
		std::vector<HistorySample> vecSamples;
		bool bGpuFound = false;   // mirrors GpuState::bGpuFound as of the latest poll
		bool bHwmonFound = false; // mirrors GpuState::bHwmonFound as of the latest poll
	};

	// Gates whether the background thread is appending to the 60-second
	// history right now. This is issue #40's explicit requirement: the
	// Statistics tab collects while it is the *selected* System Monitor
	// sub-tab, independent of the settings overlay's own open/closed
	// state -- not while the overlay happens to be visible. Call this
	// unconditionally every frame with the current "is Statistics
	// selected" value (FpsDisplay_AddLayer() does, from the persisted
	// config field, so a persisted "statistics" selection resumes
	// collecting from shortly after process start, before the panel is
	// ever drawn).
	//
	// A false->true transition clears the history and starts a fresh
	// warm-up -- re-selecting the tab after tabbing away always begins
	// empty, never resumes a stale or gapped trace (explicit acceptance
	// criterion). A true->false transition deliberately leaves the buffer
	// alone (the *next* selection is what resets it); repeated calls with
	// the same value are a cheap no-op (one atomic compare-exchange).
	// Safe to call from any thread.
	void SetHistoryCollectionEnabled( bool bEnabled );

	// Non-blocking snapshot copy, safe every frame -- mirrors Get*State().
	HistorySnapshot GetHistorySnapshot();
}
