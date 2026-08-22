#include "Volume.h"

#include "../Utils/Process.h"
#include "../log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <chrono>
#include <unordered_map>

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

		// ---- Candidate selection ---------------------------------------

		namespace
		{
			bool EqualsCI( std::string_view a, std::string_view b )
			{
				if ( a.size() != b.size() )
					return false;
				for ( size_t i = 0; i < a.size(); i++ )
					if ( std::tolower( (unsigned char)a[ i ] ) != std::tolower( (unsigned char)b[ i ] ) )
						return false;
				return true;
			}

			bool ContainsCI( const std::vector<std::string> &vecHaystack, const std::string &sNeedle )
			{
				if ( sNeedle.empty() )
					return false;
				for ( const std::string &sItem : vecHaystack )
					if ( EqualsCI( sItem, sNeedle ) )
						return true;
				return false;
			}

			long SerialOr( const NodeInfo &node )
			{
				return node.olSerial.value_or( -1 );
			}

			// The identity a node is grouped/matched by once PID matching is
			// off the table: application.process.binary if the client set
			// it, else application.name, else empty (no stable identity -
			// such a node can only ever be reached by the "newest" tier, on
			// its own).
			std::string IdentityKey( const NodeInfo &node )
			{
				return !node.sBinary.empty() ? node.sBinary : node.sAppName;
			}
		}

		SelectionResult SelectCandidate(
			const std::vector<NodeInfo> &nodes,
			const std::vector<pid_t> &vecDescendantPids,
			const std::vector<std::string> &vecDescendantNames,
			std::optional<long> olBaselineSerial,
			const std::optional<std::string> &osManualBinary )
		{
			SelectionResult result;

			// Tier 0: manual override. Always wins outright, and never
			// silently falls back to a heuristic if the chosen stream isn't
			// currently present - that's reported via an empty
			// onSelectedNodeId (VolumeState::bManualSelectionStale), not
			// replaced by a guess the user didn't ask for.
			if ( osManualBinary && !osManualBinary->empty() )
			{
				result.eMethod = DetectionMethod::Manual;
				for ( const NodeInfo &node : nodes )
				{
					if ( ( !node.sBinary.empty() && EqualsCI( node.sBinary, *osManualBinary ) ) ||
					     ( !node.sAppName.empty() && EqualsCI( node.sAppName, *osManualBinary ) ) )
						result.vecMatchedNodeIds.push_back( node.nNodeId );
				}

				if ( !result.vecMatchedNodeIds.empty() )
				{
					result.nCandidatesAtWinningTier = (int)result.vecMatchedNodeIds.size();
					const NodeInfo *pBest = nullptr;
					for ( const NodeInfo &node : nodes )
					{
						if ( std::find( result.vecMatchedNodeIds.begin(), result.vecMatchedNodeIds.end(), node.nNodeId ) == result.vecMatchedNodeIds.end() )
							continue;
						if ( !pBest || SerialOr( node ) > SerialOr( *pBest ) )
							pBest = &node;
					}
					result.onSelectedNodeId = pBest->nNodeId;
				}
				return result;
			}

			// Tier 1: PID-tree match - highest confidence when it works,
			// but exactly the one broken by pressure-vessel/bwrap PID
			// namespace sandboxing.
			{
				std::vector<pid_t> vecDistinctPids;
				for ( const NodeInfo &node : nodes )
				{
					if ( !node.onPid )
						continue;
					if ( std::find( vecDescendantPids.begin(), vecDescendantPids.end(), *node.onPid ) == vecDescendantPids.end() )
						continue;
					if ( std::find( vecDistinctPids.begin(), vecDistinctPids.end(), *node.onPid ) == vecDistinctPids.end() )
						vecDistinctPids.push_back( *node.onPid );
				}

				if ( !vecDistinctPids.empty() )
				{
					pid_t nWinningPid = vecDistinctPids.front();
					long lWinningSerial = -2;
					for ( pid_t nPid : vecDistinctPids )
					{
						long lBest = -1;
						for ( const NodeInfo &node : nodes )
							if ( node.onPid && *node.onPid == nPid )
								lBest = std::max( lBest, SerialOr( node ) );
						if ( lBest > lWinningSerial )
						{
							lWinningSerial = lBest;
							nWinningPid = nPid;
						}
					}

					result.eMethod = DetectionMethod::Pid;
					result.nCandidatesAtWinningTier = (int)vecDistinctPids.size();
					const NodeInfo *pBest = nullptr;
					for ( const NodeInfo &node : nodes )
					{
						if ( !node.onPid || *node.onPid != nWinningPid )
							continue;
						result.vecMatchedNodeIds.push_back( node.nNodeId );
						if ( !pBest || SerialOr( node ) > SerialOr( *pBest ) )
							pBest = &node;
					}
					result.onSelectedNodeId = pBest->nNodeId;
					return result;
				}
			}

			// Tier 2: process-name match - application.process.binary /
			// application.name against a descendant's own /proc comm.
			// Covers the sandboxed case: the PID doesn't line up, but the
			// process's own self-reported name does.
			{
				std::vector<std::string> vecDistinctIdentities;
				for ( const NodeInfo &node : nodes )
				{
					bool bMatch = ( !node.sBinary.empty() && ContainsCI( vecDescendantNames, node.sBinary ) ) ||
					              ( !node.sAppName.empty() && ContainsCI( vecDescendantNames, node.sAppName ) );
					if ( !bMatch )
						continue;
					std::string sKey = IdentityKey( node );
					if ( sKey.empty() )
						continue;
					if ( !ContainsCI( vecDistinctIdentities, sKey ) )
						vecDistinctIdentities.push_back( sKey );
				}

				if ( !vecDistinctIdentities.empty() )
				{
					std::string sWinningKey = vecDistinctIdentities.front();
					long lWinningSerial = -2;
					for ( const std::string &sKey : vecDistinctIdentities )
					{
						long lBest = -1;
						for ( const NodeInfo &node : nodes )
							if ( EqualsCI( IdentityKey( node ), sKey ) )
								lBest = std::max( lBest, SerialOr( node ) );
						if ( lBest > lWinningSerial )
						{
							lWinningSerial = lBest;
							sWinningKey = sKey;
						}
					}

					result.eMethod = DetectionMethod::ProcessName;
					result.nCandidatesAtWinningTier = (int)vecDistinctIdentities.size();
					const NodeInfo *pBest = nullptr;
					for ( const NodeInfo &node : nodes )
					{
						if ( !EqualsCI( IdentityKey( node ), sWinningKey ) )
							continue;
						result.vecMatchedNodeIds.push_back( node.nNodeId );
						if ( !pBest || SerialOr( node ) > SerialOr( *pBest ) )
							pBest = &node;
					}
					result.onSelectedNodeId = pBest->nNodeId;
					return result;
				}
			}

			// Tier 3: "newest stream since launch" - last resort. Strong
			// specifically because the common real case is "the hosted game
			// is the only new audio stream." Skipped entirely until a
			// baseline exists (SetTargetPid() establishes one on the first
			// poll after the target changes) - otherwise every pre-existing
			// stream would look "new."
			if ( olBaselineSerial.has_value() )
			{
				std::vector<const NodeInfo *> vecNew;
				for ( const NodeInfo &node : nodes )
					if ( node.olSerial && *node.olSerial > *olBaselineSerial )
						vecNew.push_back( &node );

				if ( !vecNew.empty() )
				{
					const NodeInfo *pBest = vecNew.front();
					for ( const NodeInfo *pNode : vecNew )
						if ( SerialOr( *pNode ) > SerialOr( *pBest ) )
							pBest = pNode;

					result.eMethod = DetectionMethod::Newest;
					result.nCandidatesAtWinningTier = (int)vecNew.size();
					std::string sWinningKey = IdentityKey( *pBest );
					for ( const NodeInfo *pNode : vecNew )
					{
						if ( !sWinningKey.empty() )
						{
							if ( !EqualsCI( IdentityKey( *pNode ), sWinningKey ) )
								continue;
						}
						else if ( pNode != pBest )
						{
							// No stable identity to group by - only the
							// single most-recent node counts, not every
							// other identity-less node that happens to
							// also be "new."
							continue;
						}
						result.vecMatchedNodeIds.push_back( pNode->nNodeId );
					}
					result.onSelectedNodeId = pBest->nNodeId;
					return result;
				}
			}

			return result; // DetectionMethod::NotDetected
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

		// Best-effort /proc/<pid>/comm read, used only for the process-name
		// detection tier - a missing/unreadable entry (pid already exited)
		// just means that pid contributes no name, not an error.
		std::optional<std::string> ReadProcComm( pid_t nPid )
		{
			char szPath[ 64 ];
			snprintf( szPath, sizeof( szPath ), "/proc/%d/comm", (int)nPid );
			FILE *pFile = fopen( szPath, "r" );
			if ( !pFile )
				return std::nullopt;

			char szBuf[ 256 ];
			size_t nRead = fread( szBuf, 1, sizeof( szBuf ) - 1, pFile );
			fclose( pFile );
			if ( nRead == 0 )
				return std::nullopt;

			szBuf[ nRead ] = '\0';
			std::string sName = szBuf;
			while ( !sName.empty() && ( sName.back() == '\n' || sName.back() == '\r' ) )
				sName.pop_back();
			return sName.empty() ? std::nullopt : std::optional<std::string>( sName );
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

		// Per-node commands for the Audio panel's "every other active
		// stream" rows - each targets one specific node id directly,
		// independent of whatever g_PendingCommand/detection resolves as
		// the primary stream this poll. Keyed by node id; drained (and,
		// like g_PendingCommand, cleared) once per poll tick.
		std::mutex g_PendingNodeMutex;
		std::unordered_map<int, PendingCommand> g_PendingNodeCommands;

		std::mutex g_StateMutex;
		VolumeState g_State;
		std::vector<StreamCandidate> g_AvailableStreams;

		std::atomic<pid_t> g_nTargetRootPid{ 0 };

		std::mutex g_ManualMutex;
		std::optional<std::string> g_osManualSelection;

		void CollectDescendants( pid_t nPid, std::vector<pid_t> &vecPidsOut, std::vector<std::string> &vecNamesOut, int nDepth = 0 )
		{
			// Bound the recursion - a runaway process tree shouldn't be
			// able to wedge this thread; real game trees are a handful
			// deep at most.
			if ( nDepth > 32 )
				return;

			vecPidsOut.push_back( nPid );
			if ( std::optional<std::string> osName = ReadProcComm( nPid ) )
				vecNamesOut.push_back( std::move( *osName ) );

			for ( pid_t nChild : gamescope::Process::GetChildPids( nPid ) )
				CollectDescendants( nChild, vecPidsOut, vecNamesOut, nDepth + 1 );
		}

		void PollThreadMain()
		{
			// One-time availability probe, done here (off the caller) so a
			// missing wpctl is confirmed and reported before anything else
			// touches it.
			RunWpctl( "--help" );

			pid_t nLastSeenTargetPid = 0;
			std::optional<long> olBaselineSerial;

			for ( ;; )
			{
				PendingCommand cmd;
				{
					std::lock_guard<std::mutex> lock( g_PendingMutex );
					cmd = g_PendingCommand;
					g_PendingCommand = {};
				}

				std::unordered_map<int, PendingCommand> nodeCommands;
				{
					std::lock_guard<std::mutex> lock( g_PendingNodeMutex );
					nodeCommands = std::move( g_PendingNodeCommands );
					g_PendingNodeCommands.clear();
				}

				std::optional<std::string> osManualSelection;
				{
					std::lock_guard<std::mutex> lock( g_ManualMutex );
					osManualSelection = g_osManualSelection;
				}

				VolumeState newState;
				newState.bWpctlAvailable = !g_bWpctlUnavailable.load( std::memory_order_relaxed );
				newState.sManualSelection = osManualSelection.value_or( std::string{} );

				pid_t nRootPid = g_nTargetRootPid.load( std::memory_order_relaxed );
				if ( nRootPid != nLastSeenTargetPid )
				{
					// A new target means "newest since launch" (tier 3)
					// needs a fresh baseline - established below, from
					// this same poll's node snapshot, so nothing that
					// already existed can ever count as "new."
					nLastSeenTargetPid = nRootPid;
					olBaselineSerial.reset();
				}

				std::vector<StreamCandidate> vecAvailable;

				if ( newState.bWpctlAvailable )
				{
					std::optional<std::string> osStatus = RunWpctl( "status" );
					if ( osStatus )
					{
						std::vector<Parse::NodeInfo> vecNodes;

						for ( const auto &[ nNodeId, sName ] : Parse::StatusStreamNodes( *osStatus ) )
						{
							std::optional<std::string> osInspect = RunWpctl( "inspect " + std::to_string( nNodeId ) );
							if ( !osInspect || !Parse::IsAudioOutputStream( *osInspect ) )
								continue;

							Parse::NodeInfo node;
							node.nNodeId = nNodeId;
							node.sBinary = Parse::InspectField( *osInspect, "application.process.binary" ).value_or( std::string{} );
							node.sAppName = Parse::InspectField( *osInspect, "application.name" ).value_or( std::string{} );
							if ( std::optional<std::string> osPid = Parse::InspectField( *osInspect, "application.process.id" ) )
								node.onPid = (pid_t)std::atoi( osPid->c_str() );
							if ( std::optional<std::string> osSerial = Parse::InspectField( *osInspect, "object.serial" ) )
								node.olSerial = std::atol( osSerial->c_str() );

							// Apply this node's own pending command (if any)
							// before reading its volume back below, so a
							// change made through this row's own slider is
							// reflected in this same poll tick rather than
							// waiting a further 750ms.
							auto itNodeCmd = nodeCommands.find( nNodeId );
							if ( itNodeCmd != nodeCommands.end() )
							{
								if ( itNodeCmd->second.oflLinearVolume )
									RunWpctl( "set-volume " + std::to_string( nNodeId ) + " " + Parse::FormatLinearVolume( *itNodeCmd->second.oflLinearVolume ) );
								if ( itNodeCmd->second.obMuted )
									RunWpctl( "set-mute " + std::to_string( nNodeId ) + " " + ( *itNodeCmd->second.obMuted ? "1" : "0" ) );
							}

							StreamCandidate candidate;
							candidate.nNodeId = nNodeId;
							candidate.sLabel = sName;
							candidate.sBinary = node.sBinary;
							candidate.sAppName = node.sAppName;
							candidate.nPid = node.onPid.value_or( 0 );

							// Every row in the Audio panel's stream list
							// binds straight to this - a per-node live
							// readback, not a second GetState()-shaped call.
							if ( std::optional<std::string> osVol = RunWpctl( "get-volume " + std::to_string( nNodeId ) ) )
							{
								if ( auto oReading = Parse::GetVolumeOutput( *osVol ) )
								{
									candidate.flVolume = LinearToDisplayVolume( oReading->flLinear );
									candidate.bMuted = oReading->bMuted;
								}
							}

							vecAvailable.push_back( candidate );
							vecNodes.push_back( std::move( node ) );
						}

						newState.nTotalAudioStreams = (int)vecNodes.size();

						if ( !olBaselineSerial.has_value() )
						{
							// First poll for this target: the baseline is
							// "whatever already exists," so tier 3 only
							// ever matches streams that appear afterward.
							long lMax = -1;
							for ( const Parse::NodeInfo &node : vecNodes )
								lMax = std::max( lMax, node.olSerial.value_or( -1 ) );
							olBaselineSerial = lMax;
						}

						std::vector<pid_t> vecDescendants;
						std::vector<std::string> vecDescendantNames;
						if ( nRootPid > 0 )
							CollectDescendants( nRootPid, vecDescendants, vecDescendantNames );

						Parse::SelectionResult selection = Parse::SelectCandidate(
							vecNodes, vecDescendants, vecDescendantNames, olBaselineSerial, osManualSelection );

						newState.eMethod = selection.eMethod;
						newState.bDetected = selection.onSelectedNodeId.has_value();
						newState.nMatchedNodes = (int)selection.vecMatchedNodeIds.size();
						newState.vecMatchedNodeIds = selection.vecMatchedNodeIds;
						newState.nCandidatesAtWinningTier = selection.nCandidatesAtWinningTier;
						newState.bManualSelectionStale = osManualSelection.has_value() && !osManualSelection->empty() && !newState.bDetected;

						// Apply pending commands directly by node id, never
						// via `--pid` - that's exactly the path a PID
						// namespace mismatch breaks, so it would silently
						// undo everything the process-name/newest tiers
						// exist to fix.
						if ( !selection.vecMatchedNodeIds.empty() && ( cmd.oflLinearVolume || cmd.obMuted ) )
						{
							for ( int nNodeId : selection.vecMatchedNodeIds )
							{
								if ( cmd.oflLinearVolume )
									RunWpctl( "set-volume " + std::to_string( nNodeId ) + " " + Parse::FormatLinearVolume( *cmd.oflLinearVolume ) );
								if ( cmd.obMuted )
									RunWpctl( "set-mute " + std::to_string( nNodeId ) + " " + ( *cmd.obMuted ? "1" : "0" ) );
							}
						}

						if ( selection.onSelectedNodeId )
						{
							std::optional<std::string> osVol = RunWpctl( "get-volume " + std::to_string( *selection.onSelectedNodeId ) );
							if ( osVol )
							{
								if ( auto oReading = Parse::GetVolumeOutput( *osVol ) )
								{
									newState.flVolume = LinearToDisplayVolume( oReading->flLinear );
									newState.bMuted = oReading->bMuted;

									// The primary command (if any) was
									// applied above, after this node's own
									// candidate.flVolume/bMuted was already
									// read - keep vecAvailable's copy of the
									// primary node in sync so it isn't
									// stale for one extra poll tick if it's
									// ever also listed as an "other" row.
									for ( StreamCandidate &candidate : vecAvailable )
									{
										if ( candidate.nNodeId == *selection.onSelectedNodeId )
										{
											candidate.flVolume = newState.flVolume;
											candidate.bMuted = newState.bMuted;
											break;
										}
									}
								}
							}
						}
					}
				}

				{
					std::lock_guard<std::mutex> lock( g_StateMutex );
					g_State = newState;
					g_AvailableStreams = std::move( vecAvailable );
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

	std::vector<StreamCandidate> GetAvailableStreams()
	{
		std::lock_guard<std::mutex> lock( g_StateMutex );
		return g_AvailableStreams;
	}

	void SetManualSelection( std::optional<std::string> sBinaryOrAppName )
	{
		std::lock_guard<std::mutex> lock( g_ManualMutex );
		if ( sBinaryOrAppName && sBinaryOrAppName->empty() )
			sBinaryOrAppName.reset();
		g_osManualSelection = std::move( sBinaryOrAppName );
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

	void RequestVolumeForNode( int nNodeId, float flDisplayFraction )
	{
		std::lock_guard<std::mutex> lock( g_PendingNodeMutex );
		g_PendingNodeCommands[ nNodeId ].oflLinearVolume = DisplayToLinearVolume( flDisplayFraction );
	}

	void RequestMuteForNode( int nNodeId, bool bMuted )
	{
		std::lock_guard<std::mutex> lock( g_PendingNodeMutex );
		g_PendingNodeCommands[ nNodeId ].obMuted = bMuted;
	}
}
