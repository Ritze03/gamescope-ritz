// See SystemStats.h for the data-source/threading rationale.
#include "SystemStats.h"

#include "../log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>

namespace gamescope::Metrics
{
	static LogScope s_MetricsLog( "system_stats" );

	// ---- Poll cadence -------------------------------------------------------
	// CPU/GPU sysfs reads are plain small-file reads (negligible cost), polled
	// at 2Hz per the research doc's own "once or twice a second is standard
	// for this kind of display" recommendation -- fast enough that busy%/load
	// still feels live, far below vblank-rate so it never matters against the
	// render loop. Media polling forks a `playerctl` subprocess (the one
	// source here that's actually expensive), so it runs on its own much
	// slower cadence -- track metadata changes rarely, and forking at 2Hz
	// would be wasteful for something that only changes on a song/pause
	// change.
	static constexpr int kPollIntervalMs = 500;                  // 2Hz: CPU/GPU sysfs
	static constexpr int kMediaPollDivisorTicks = 4;              // every 4th tick = ~2s: media (playerctl)

	// ---- Shared snapshot state, mirroring Audio/Volume.cpp's g_StateMutex/
	// g_State mailbox pattern exactly. -----------------------------------------
	namespace
	{
		std::mutex g_StateMutex;
		CpuState g_CpuState;
		GpuState g_GpuState;
		MediaState g_MediaState;

		std::atomic<bool> g_bInitStarted{ false };

		// ---- Small-file sysfs/proc helpers ------------------------------------

		// Reads a whole small file (sysfs/proc node) into sOut. false on any
		// failure (missing file, permission, etc.) -- every caller treats
		// that as "this data point is unavailable right now," never a crash.
		bool ReadSmallFile( const std::string &sPath, std::string &sOut )
		{
			FILE *pFile = fopen( sPath.c_str(), "r" );
			if ( !pFile )
				return false;

			char szBuf[ 4096 ];
			size_t nRead = fread( szBuf, 1, sizeof( szBuf ) - 1, pFile );
			fclose( pFile );
			if ( nRead == 0 )
				return false;

			szBuf[ nRead ] = '\0';
			sOut.assign( szBuf, nRead );
			return true;
		}

		std::string Trim( std::string s )
		{
			while ( !s.empty() && ( s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t' ) )
				s.pop_back();
			size_t nStart = 0;
			while ( nStart < s.size() && ( s[ nStart ] == ' ' || s[ nStart ] == '\t' ) )
				++nStart;
			return s.substr( nStart );
		}

		bool ReadUint64File( const std::string &sPath, uint64_t &ulOut )
		{
			std::string sContent;
			if ( !ReadSmallFile( sPath, sContent ) )
				return false;
			ulOut = strtoull( sContent.c_str(), nullptr, 10 );
			return true;
		}

		bool PathReadable( const std::string &sPath )
		{
			return access( sPath.c_str(), R_OK ) == 0;
		}

		// ---- CPU / RAM ----------------------------------------------------------

		CpuState PollCpu()
		{
			CpuState state;

			std::string sLoadAvg;
			if ( ReadSmallFile( "/proc/loadavg", sLoadAvg ) )
			{
				float flLoad1 = 0.0f, flLoad5 = 0.0f, flLoad15 = 0.0f;
				if ( sscanf( sLoadAvg.c_str(), "%f %f %f", &flLoad1, &flLoad5, &flLoad15 ) == 3 )
				{
					state.bLoadAvailable = true;
					state.flLoad1 = flLoad1;
					state.flLoad5 = flLoad5;
					state.flLoad15 = flLoad15;
				}
			}

			std::string sMemInfo;
			if ( ReadSmallFile( "/proc/meminfo", sMemInfo ) )
			{
				uint64_t ulTotalKb = 0, ulAvailKb = 0;
				bool bHaveTotal = false, bHaveAvail = false;

				size_t nPos = 0;
				while ( nPos < sMemInfo.size() )
				{
					size_t nEol = sMemInfo.find( '\n', nPos );
					if ( nEol == std::string::npos )
						nEol = sMemInfo.size();
					std::string_view svLine( sMemInfo.data() + nPos, nEol - nPos );

					if ( !bHaveTotal && svLine.rfind( "MemTotal:", 0 ) == 0 )
					{
						ulTotalKb = strtoull( svLine.data() + 9, nullptr, 10 );
						bHaveTotal = true;
					}
					else if ( !bHaveAvail && svLine.rfind( "MemAvailable:", 0 ) == 0 )
					{
						ulAvailKb = strtoull( svLine.data() + 13, nullptr, 10 );
						bHaveAvail = true;
					}

					if ( bHaveTotal && bHaveAvail )
						break;
					nPos = nEol + 1;
				}

				if ( bHaveTotal && bHaveAvail )
				{
					state.bMemAvailable = true;
					state.ulMemTotalKb = ulTotalKb;
					state.ulMemUsedKb = ulTotalKb > ulAvailKb ? ulTotalKb - ulAvailKb : 0;
				}
			}

			return state;
		}

		// ---- AMD GPU sysfs path resolution --------------------------------------
		// Resolved once at thread start, not every poll -- these are stable
		// device paths for the lifetime of the session (sysfs indices don't
		// migrate without a GPU hot-unplug, which this doesn't try to handle
		// live). A missing amdgpu node (any non-AMD GPU, or an AMD card whose
		// driver hasn't bound) just leaves bGpuFound/bHwmonFound false --
		// this issue's own explicit requirement that the GPU module degrade
		// gracefully rather than crash or show garbage on other hardware.

		struct GpuPaths
		{
			bool bGpuFound = false;
			std::string sBusyPercentPath;
			std::string sVramUsedPath;
			std::string sVramTotalPath;

			bool bHwmonFound = false;
			std::string sTempPath;
			std::string sPowerPath;
		};

		// Finds the first /sys/class/drm/cardN (exact "cardN", not a
		// connector sub-node like "cardN-DP-1") whose device exposes
		// gpu_busy_percent -- the amdgpu-specific whole-device usage node
		// this issue's research doc verified. Never hardcodes a card index:
		// the doc explicitly calls out that N is not portable across
		// machines/reboots.
		void ResolveDrmPaths( GpuPaths &paths )
		{
			DIR *pDir = opendir( "/sys/class/drm" );
			if ( !pDir )
				return;

			struct dirent *pEnt;
			while ( ( pEnt = readdir( pDir ) ) != nullptr )
			{
				std::string_view svName = pEnt->d_name;
				if ( svName.size() < 5 || svName.substr( 0, 4 ) != "card" )
					continue;
				if ( svName.find( '-' ) != std::string_view::npos )
					continue; // e.g. "card1-DP-1" -- a connector, not the device itself

				std::string sBase = std::string( "/sys/class/drm/" ) + pEnt->d_name + "/device/";
				std::string sBusy = sBase + "gpu_busy_percent";
				if ( PathReadable( sBusy ) )
				{
					paths.bGpuFound = true;
					paths.sBusyPercentPath = sBusy;
					paths.sVramUsedPath = sBase + "mem_info_vram_used";
					paths.sVramTotalPath = sBase + "mem_info_vram_total";
					break;
				}
			}
			closedir( pDir );
		}

		// Scans /sys/class/hwmon/hwmon*/name for the entry named "amdgpu" --
		// the hwmonN index is not portable (this issue's own explicit
		// requirement), so it's resolved by content match, never hardcoded.
		void ResolveHwmonPaths( GpuPaths &paths )
		{
			DIR *pDir = opendir( "/sys/class/hwmon" );
			if ( !pDir )
				return;

			struct dirent *pEnt;
			while ( ( pEnt = readdir( pDir ) ) != nullptr )
			{
				std::string_view svName = pEnt->d_name;
				if ( svName.substr( 0, 5 ) != "hwmon" )
					continue;

				std::string sBase = std::string( "/sys/class/hwmon/" ) + pEnt->d_name + "/";
				std::string sNameContent;
				if ( !ReadSmallFile( sBase + "name", sNameContent ) )
					continue;
				if ( Trim( sNameContent ) != "amdgpu" )
					continue;

				paths.bHwmonFound = true;

				// Prefer the tempN_input whose matching tempN_label reads
				// "edge" (the GPU die's own sensor, the reading users
				// actually recognize from `sensors`/MangoHud); fall back to
				// temp1_input if no label matches (still a real GPU-adjacent
				// reading, just not confirmed as specifically "edge").
				paths.sTempPath = sBase + "temp1_input";
				for ( int i = 1; i <= 8; ++i )
				{
					std::string sLabelPath = sBase + "temp" + std::to_string( i ) + "_label";
					std::string sLabel;
					if ( ReadSmallFile( sLabelPath, sLabel ) && Trim( sLabel ) == "edge" )
					{
						paths.sTempPath = sBase + "temp" + std::to_string( i ) + "_input";
						break;
					}
				}

				// power1_average (µW, the reading most amdgpu cards expose)
				// with power1_input as a fallback for cards that only
				// expose the instantaneous reading.
				std::string sAvgPath = sBase + "power1_average";
				paths.sPowerPath = PathReadable( sAvgPath ) ? sAvgPath : sBase + "power1_input";

				break;
			}
			closedir( pDir );
		}

		GpuPaths ResolveGpuPaths()
		{
			GpuPaths paths;
			ResolveDrmPaths( paths );
			ResolveHwmonPaths( paths );

			if ( !paths.bGpuFound )
				s_MetricsLog.warnf( "no amdgpu DRM device (gpu_busy_percent) found -- GPU module will report itself unavailable (expected on non-AMD hardware)" );
			else if ( !paths.bHwmonFound )
				s_MetricsLog.warnf( "amdgpu DRM device found but no amdgpu hwmon node -- GPU temp/power will be unavailable" );

			return paths;
		}

		GpuState PollGpu( const GpuPaths &paths )
		{
			GpuState state;

			state.bGpuFound = paths.bGpuFound;
			if ( paths.bGpuFound )
			{
				std::string sBusy;
				if ( ReadSmallFile( paths.sBusyPercentPath, sBusy ) )
					state.nBusyPercent = std::clamp( atoi( sBusy.c_str() ), 0, 100 );

				ReadUint64File( paths.sVramUsedPath, state.ulVramUsedBytes );
				ReadUint64File( paths.sVramTotalPath, state.ulVramTotalBytes );
			}

			state.bHwmonFound = paths.bHwmonFound;
			if ( paths.bHwmonFound )
			{
				uint64_t ulTempMilliC = 0;
				if ( ReadUint64File( paths.sTempPath, ulTempMilliC ) )
					state.flTempC = (float)ulTempMilliC / 1000.0f;

				uint64_t ulPowerMicroW = 0;
				if ( ReadUint64File( paths.sPowerPath, ulPowerMicroW ) )
					state.flPowerWatts = (float)ulPowerMicroW / 1'000'000.0f;
			}

			return state;
		}

		// ---- Media playback (`playerctl` shell-out) -----------------------------
		// Same popen() shape as Audio/Volume.cpp's RunWpctl() -- see that
		// function's comment for why building the command via popen()'s
		// shell is safe here (the whole command line is a fixed literal;
		// nothing external is spliced into it).

		std::atomic<bool> g_bPlayerctlUnavailable{ false };

		// Runs a playerctl subcommand and returns trimmed stdout, or nullopt
		// on any failure (including a confirmed-missing binary, at which
		// point every later call short-circuits without spawning anything --
		// same "don't spam retries for a permanently missing tool" behavior
		// as Volume.cpp's RunWpctl()).
		std::optional<std::string> RunPlayerctl( const char *pszArgs )
		{
			if ( g_bPlayerctlUnavailable.load( std::memory_order_relaxed ) )
				return std::nullopt;

			std::string sCmd = std::string( "playerctl " ) + pszArgs + " 2>/dev/null";
			FILE *pFile = popen( sCmd.c_str(), "r" );
			if ( !pFile )
				return std::nullopt;

			std::string sOutput;
			char szBuf[ 1024 ];
			size_t nRead;
			while ( ( nRead = fread( szBuf, 1, sizeof( szBuf ), pFile ) ) > 0 )
				sOutput.append( szBuf, nRead );

			int nStatus = pclose( pFile );
			if ( nStatus == -1 )
				return std::nullopt;

			if ( WIFEXITED( nStatus ) && WEXITSTATUS( nStatus ) == 127 )
			{
				g_bPlayerctlUnavailable.store( true, std::memory_order_relaxed );
				s_MetricsLog.warnf( "playerctl not found -- disabling the media module for this session" );
				return std::nullopt;
			}

			// Any other nonzero exit (most commonly: "No players found",
			// exit 1) means "nothing playing right now," not an error --
			// handled by the caller via an empty MediaState, not logged.
			if ( !WIFEXITED( nStatus ) || WEXITSTATUS( nStatus ) != 0 )
				return std::nullopt;

			return Trim( sOutput );
		}

		MediaState PollMedia()
		{
			MediaState state;

			std::optional<std::string> osStatus = RunPlayerctl( "status" );
			if ( !osStatus || osStatus->empty() )
				return state; // no MPRIS player currently running -- an honestly-empty module, not an error

			state.bPlayerAvailable = true;
			if ( *osStatus == "Playing" )
				state.eStatus = PlaybackStatus::Playing;
			else if ( *osStatus == "Paused" )
				state.eStatus = PlaybackStatus::Paused;
			else if ( *osStatus == "Stopped" )
				state.eStatus = PlaybackStatus::Stopped;

			if ( std::optional<std::string> osTitle = RunPlayerctl( "metadata title" ) )
				state.sTitle = std::move( *osTitle );
			if ( std::optional<std::string> osArtist = RunPlayerctl( "metadata artist" ) )
				state.sArtist = std::move( *osArtist );

			return state;
		}

		// ---- Poll thread --------------------------------------------------------

		void PollThreadMain()
		{
			GpuPaths gpuPaths = ResolveGpuPaths();

			int nTick = 0;
			for ( ;; )
			{
				CpuState cpu = PollCpu();
				GpuState gpu = PollGpu( gpuPaths );

				if ( nTick % kMediaPollDivisorTicks == 0 )
				{
					MediaState media = PollMedia();
					std::lock_guard<std::mutex> lock( g_StateMutex );
					g_CpuState = cpu;
					g_GpuState = gpu;
					g_MediaState = media;
				}
				else
				{
					std::lock_guard<std::mutex> lock( g_StateMutex );
					g_CpuState = cpu;
					g_GpuState = gpu;
				}

				++nTick;
				std::this_thread::sleep_for( std::chrono::milliseconds( kPollIntervalMs ) );
			}
		}
	}

	void Init()
	{
		if ( g_bInitStarted.exchange( true ) )
			return;

		std::thread thread( PollThreadMain );
		thread.detach();
	}

	CpuState GetCpuState()
	{
		std::lock_guard<std::mutex> lock( g_StateMutex );
		return g_CpuState;
	}

	GpuState GetGpuState()
	{
		std::lock_guard<std::mutex> lock( g_StateMutex );
		return g_GpuState;
	}

	MediaState GetMediaState()
	{
		std::lock_guard<std::mutex> lock( g_StateMutex );
		return g_MediaState;
	}
}
