#include "Volume.h"

#include "../Utils/Process.h"
#include "../log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <chrono>

#include <sys/wait.h>

static LogScope s_AudioLog( "audio_volume" );

namespace gamescope::Audio
{
	// ---- Parsing helpers (pure, unit-tested directly) ----------------------

	namespace Parse
	{
		std::vector<std::pair<int, std::string>> StatusStreamNodes( std::string_view svStatusOutput )
		{
			std::vector<std::pair<int, std::string>> nodes;
			bool bInStreams = false;

			size_t nPos = 0;
			while ( nPos <= svStatusOutput.size() )
			{
				size_t nNewline = svStatusOutput.find( '\n', nPos );
				std::string_view svLine = ( nNewline == std::string_view::npos )
					? svStatusOutput.substr( nPos )
					: svStatusOutput.substr( nPos, nNewline - nPos );

				size_t nContentStart = svLine.find_first_not_of( " \t\xE2\x94\x82\xE2\x94\x9C\xE2\x94\x94\xE2\x94\x80" );
				std::string_view svTrimmed = ( nContentStart == std::string_view::npos ) ? std::string_view{} : svLine.substr( nContentStart );

				if ( bInStreams )
				{
					if ( svTrimmed.empty() )
					{
						bInStreams = false;
					}
					else if ( svTrimmed.find( " > " ) == std::string_view::npos )
					{
						// Node lines look like "281. paplay"; port sub-lines
						// (skipped above) look like
						// "230. output_FL > Easy Effects Sink:playback_FL [active]".
						size_t nDot = svTrimmed.find( '.' );
						if ( nDot != std::string_view::npos && nDot > 0 &&
						     svTrimmed.substr( 0, nDot ).find_first_not_of( "0123456789" ) == std::string_view::npos )
						{
							int nId = std::atoi( std::string( svTrimmed.substr( 0, nDot ) ).c_str() );

							size_t nNameStart = svTrimmed.find_first_not_of( " \t", nDot + 1 );
							std::string sName = ( nNameStart == std::string_view::npos ) ? std::string{} : std::string( svTrimmed.substr( nNameStart ) );
							while ( !sName.empty() && ( sName.back() == ' ' || sName.back() == '\t' || sName.back() == '\r' ) )
								sName.pop_back();

							nodes.emplace_back( nId, std::move( sName ) );
						}
					}
				}
				else if ( svTrimmed.size() >= 8 && svTrimmed.substr( svTrimmed.size() - 8 ) == "Streams:" )
				{
					bInStreams = true;
				}

				if ( nNewline == std::string_view::npos )
					break;
				nPos = nNewline + 1;
			}

			return nodes;
		}

		std::optional<std::string> InspectField( std::string_view svInspectOutput, std::string_view svKey )
		{
			size_t nPos = 0;
			while ( nPos < svInspectOutput.size() )
			{
				size_t nNewline = svInspectOutput.find( '\n', nPos );
				std::string_view svLine = ( nNewline == std::string_view::npos ) ? svInspectOutput.substr( nPos ) : svInspectOutput.substr( nPos, nNewline - nPos );

				size_t nKeyPos = svLine.find( svKey );
				if ( nKeyPos != std::string_view::npos )
				{
					size_t nEq = svLine.find( '=', nKeyPos );
					if ( nEq != std::string_view::npos )
					{
						size_t nQ1 = svLine.find( '"', nEq );
						size_t nQ2 = ( nQ1 == std::string_view::npos ) ? std::string_view::npos : svLine.find( '"', nQ1 + 1 );
						if ( nQ1 != std::string_view::npos && nQ2 != std::string_view::npos )
							return std::string( svLine.substr( nQ1 + 1, nQ2 - nQ1 - 1 ) );
					}
				}

				if ( nNewline == std::string_view::npos )
					break;
				nPos = nNewline + 1;
			}
			return std::nullopt;
		}

		bool IsAudioOutputStream( std::string_view svInspectOutput )
		{
			std::optional<std::string> osClass = InspectField( svInspectOutput, "media.class" );
			return osClass && *osClass == "Stream/Output/Audio";
		}

		std::optional<VolumeReading> GetVolumeOutput( std::string_view svOutput )
		{
			static constexpr std::string_view k_svPrefix = "Volume:";
			size_t nPos = svOutput.find( k_svPrefix );
			if ( nPos == std::string_view::npos )
				return std::nullopt;

			nPos += k_svPrefix.size();
			while ( nPos < svOutput.size() && svOutput[ nPos ] == ' ' )
				nPos++;

			size_t nStart = nPos;
			while ( nPos < svOutput.size() && ( ( svOutput[ nPos ] >= '0' && svOutput[ nPos ] <= '9' ) || svOutput[ nPos ] == '.' ) )
				nPos++;

			if ( nPos == nStart )
				return std::nullopt;

			float flLinear = std::strtof( std::string( svOutput.substr( nStart, nPos - nStart ) ).c_str(), nullptr );
			bool bMuted = svOutput.find( "MUTED", nPos ) != std::string_view::npos;
			return VolumeReading{ flLinear, bMuted };
		}

		std::string FormatLinearVolume( float flLinear )
		{
			if ( flLinear < 0.0f )
				flLinear = 0.0f;

			// Round to 4 decimal places by hand rather than via a
			// locale-sensitive float formatter - wpctl expects a plain
			// '.'-decimal number regardless of the process locale.
			long lMilli = std::lround( flLinear * 10000.0f );
			long lWhole = lMilli / 10000;
			long lFrac = lMilli % 10000;

			char szBuf[ 32 ];
			snprintf( szBuf, sizeof( szBuf ), "%ld.%04ld", lWhole, lFrac );
			return szBuf;
		}
	}

	// ---- wpctl shell-out ----------------------------------------------------

	namespace
	{
		std::atomic<bool> g_bWpctlUnavailable{ false };

		// Runs `wpctl <sArgs>` and captures stdout. Returns nullopt on any
		// failure, including - once detected - a permanently missing wpctl
		// binary, at which point every later call short-circuits without
		// spawning anything, so a missing binary never spams retries or
		// stderr output.
		std::optional<std::string> RunWpctl( const std::string &sArgs )
		{
			if ( g_bWpctlUnavailable.load( std::memory_order_relaxed ) )
				return std::nullopt;

			// Every argument we splice in is either a fixed literal or a
			// number we formatted ourselves (pid_t, node id, FormatLinearVolume) -
			// never an arbitrary string - so building this via popen()'s
			// shell is not an injection risk.
			std::string sCmd = "wpctl " + sArgs + " 2>/dev/null";
			FILE *pFile = popen( sCmd.c_str(), "r" );
			if ( !pFile )
			{
				s_AudioLog.errorf( "Failed to spawn wpctl" );
				return std::nullopt;
			}

			std::string sOutput;
			char szBuf[ 4096 ];
			size_t nRead;
			while ( ( nRead = fread( szBuf, 1, sizeof( szBuf ), pFile ) ) > 0 )
				sOutput.append( szBuf, nRead );

			int nStatus = pclose( pFile );
			if ( nStatus == -1 )
				return std::nullopt;

			if ( WIFEXITED( nStatus ) && WEXITSTATUS( nStatus ) == 127 )
			{
				g_bWpctlUnavailable.store( true, std::memory_order_relaxed );
				s_AudioLog.errorf( "wpctl not found - disabling volume control for this session" );
				return std::nullopt;
			}

			return sOutput;
		}

		// ---- Mailbox state, mirroring the pattern pipewire.cpp uses for its
		// own dedicated PipeWire thread: a background thread does all the
		// blocking work, and hands snapshots back through a small guarded
		// slot the render/UI thread only ever copies out of. -------------------

		struct PendingCommand
		{
			std::optional<float> oflLinearVolume;
			std::optional<bool> obMuted;
		};

		std::mutex g_PendingMutex;
		PendingCommand g_PendingCommand;

		std::mutex g_StateMutex;
		VolumeState g_State;

		std::atomic<pid_t> g_nTargetRootPid{ 0 };

		void CollectDescendants( pid_t nPid, std::vector<pid_t> &vecOut, int nDepth = 0 )
		{
			// Bound the recursion - a runaway process tree shouldn't be
			// able to wedge this thread; real game trees are a handful
			// deep at most.
			if ( nDepth > 32 )
				return;

			vecOut.push_back( nPid );
			for ( pid_t nChild : gamescope::Process::GetChildPids( nPid ) )
				CollectDescendants( nChild, vecOut, nDepth + 1 );
		}

		void PollThreadMain()
		{
			// One-time availability probe, done here (off the caller) so a
			// missing wpctl is confirmed and reported before anything else
			// touches it.
			RunWpctl( "--help" );

			for ( ;; )
			{
				PendingCommand cmd;
				{
					std::lock_guard<std::mutex> lock( g_PendingMutex );
					cmd = g_PendingCommand;
					g_PendingCommand = {};
				}

				VolumeState newState;
				newState.bWpctlAvailable = !g_bWpctlUnavailable.load( std::memory_order_relaxed );

				pid_t nRootPid = g_nTargetRootPid.load( std::memory_order_relaxed );

				if ( nRootPid > 0 && newState.bWpctlAvailable )
				{
					std::vector<pid_t> vecDescendants;
					CollectDescendants( nRootPid, vecDescendants );

					std::optional<std::string> osStatus = RunWpctl( "status" );
					if ( osStatus )
					{
						std::vector<int> vecMatchedNodeIds;
						std::vector<pid_t> vecMatchedPids;

						for ( const auto &[ nNodeId, sName ] : Parse::StatusStreamNodes( *osStatus ) )
						{
							std::optional<std::string> osInspect = RunWpctl( "inspect " + std::to_string( nNodeId ) );
							if ( !osInspect || !Parse::IsAudioOutputStream( *osInspect ) )
								continue;

							std::optional<std::string> osPid = Parse::InspectField( *osInspect, "application.process.id" );
							if ( !osPid )
								continue;

							pid_t nStreamPid = (pid_t)std::atoi( osPid->c_str() );
							if ( std::find( vecDescendants.begin(), vecDescendants.end(), nStreamPid ) == vecDescendants.end() )
								continue;

							vecMatchedNodeIds.push_back( nNodeId );
							if ( std::find( vecMatchedPids.begin(), vecMatchedPids.end(), nStreamPid ) == vecMatchedPids.end() )
								vecMatchedPids.push_back( nStreamPid );
						}

						// Apply to every matched process - wpctl's --pid
						// already fans one call out to all of that
						// process's own nodes, so this is at most one call
						// per distinct matched PID, not per node.
						if ( !vecMatchedPids.empty() && ( cmd.oflLinearVolume || cmd.obMuted ) )
						{
							for ( pid_t nPid : vecMatchedPids )
							{
								if ( cmd.oflLinearVolume )
									RunWpctl( "set-volume --pid " + std::to_string( nPid ) + " " + Parse::FormatLinearVolume( *cmd.oflLinearVolume ) );
								if ( cmd.obMuted )
									RunWpctl( "set-mute --pid " + std::to_string( nPid ) + " " + ( *cmd.obMuted ? "1" : "0" ) );
							}
						}

						newState.nMatchedNodes = (int)vecMatchedNodeIds.size();
						newState.bDetected = !vecMatchedNodeIds.empty();

						if ( newState.bDetected )
						{
							std::optional<std::string> osVol = RunWpctl( "get-volume " + std::to_string( vecMatchedNodeIds.front() ) );
							if ( osVol )
							{
								if ( auto oReading = Parse::GetVolumeOutput( *osVol ) )
								{
									newState.flVolume = LinearToDisplayVolume( oReading->flLinear );
									newState.bMuted = oReading->bMuted;
								}
							}
						}
					}
				}

				{
					std::lock_guard<std::mutex> lock( g_StateMutex );
					g_State = newState;
				}

				// ponytail: fixed poll cadence rather than PipeWire registry
				// push events - fine for an overlay that isn't open 24/7 (per
				// the feasibility scout); upgrade path is a direct pw_registry
				// listener if this ever needs to feel more live.
				std::this_thread::sleep_for( std::chrono::milliseconds( 750 ) );
			}
		}
	}

	void Init()
	{
		static std::once_flag s_InitFlag;
		std::call_once( s_InitFlag, []()
		{
			std::thread thread( PollThreadMain );
			thread.detach();
		} );
	}

	void SetTargetPid( pid_t nRootPid )
	{
		g_nTargetRootPid.store( nRootPid, std::memory_order_relaxed );
	}

	VolumeState GetState()
	{
		std::lock_guard<std::mutex> lock( g_StateMutex );
		return g_State;
	}

	void RequestVolume( float flDisplayFraction )
	{
		std::lock_guard<std::mutex> lock( g_PendingMutex );
		g_PendingCommand.oflLinearVolume = DisplayToLinearVolume( flDisplayFraction );
	}

	void RequestMute( bool bMuted )
	{
		std::lock_guard<std::mutex> lock( g_PendingMutex );
		g_PendingCommand.obMuted = bMuted;
	}
}
