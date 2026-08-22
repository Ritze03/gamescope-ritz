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
}
