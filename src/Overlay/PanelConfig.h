// The Setup section's three areas of the settings overlay -- Profiles,
// Per-game and Appearance: the "Use separate settings for this game"
// switch, the profile manager (use / restore / save as new), and the
// overlay's own appearance settings. See superdoc/features/
// profiles-and-per-game.md (the model in plain words, and what is still
// pending as Phase B), superdoc/planning/SPEC.md's Feature 6 ("Config
// system"), and superdoc/planning/DECISIONS.md #19-#21.
//
// This is the only panel that ever changes *which file* the other panels
// (PanelDisplay, PanelShaders, FpsDisplay) persist their own live edits
// into -- see Config/ConfigManager.h's session-routing section
// (SessionAppId/IsSessionOverrideActive/ConfigGeneration), which this panel
// is the sole writer of.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Config/ConfigSchema.h"

namespace gamescope
{
	namespace ui { class Registry; }

	// P3 part B: the E2 registration, replacing the Escape() hatch below.
	//
	// The panel's three tabs become THREE AREAS -- setup.profiles,
	// setup.pergame and setup.appearance -- following D13.1: the rail is
	// the product's only navigation, and index.html lists exactly these
	// three as rail items. Profiles and Per-game are DYNAMIC (the profile
	// list and the set of other games with overrides both change while the
	// overlay is open); Appearance is not.
	void PanelConfig_RegisterAreas( ui::Registry &reg );

	// =====================================================================
	//  The pure half of the panel -- no ImGui, no disk, no compositor.
	// =====================================================================
	// Header-only so tests/test_overlay_profiles.cpp can hold it to account
	// without linking PanelConfig.cpp (which needs Notifications, Fonts and
	// the live overlay). Everything here is a plain function of plain
	// values; PanelConfig.cpp feeds it the panel's file-static state.
	namespace panelconfig
	{
		// The one-step safety net behind "Use this profile" and "Start from
		// profile" (requests-2026-09-05 item 3): the target file's Settings
		// exactly as they were the instant before a profile was copied over
		// them, plus enough context to know WHERE they came from.
		//
		// In memory only, never on disk -- a Use is one click, and the fear
		// being answered is "I can't get back what I had", not "I need a
		// history". A second Use replaces it: one step back, never two.
		struct SettingsBackup
		{
			config::Settings settings;         // the target as it was before the Use
			std::string sReplacedBy;           // the profile whose Use took the backup
			bool bWasOverride = false;         // routing when it was taken: per-game file or global.json
		};

		// A backup is only ever written back to the file it was taken from.
		// If the user used a profile while on global.json and then switched
		// "Use separate settings for this game" on, restoring through the
		// routed path would land global's old values in the per-game file
		// -- the wrong file. So Restore is disabled (with a reason) until
		// the routing matches again; it is never silently redirected.
		inline bool BackupMatchesRouting( const SettingsBackup &backup, bool bOverrideActiveNow )
		{
			return backup.bWasOverride == bOverrideActiveNow;
		}

		// Everything that decides which rows the two dynamic areas have and
		// what their Status rows say. Hashed by StatusHash() below, which is
		// the areas' ui::Area::Rebuilds() generation: when any of it moves,
		// the sheet is rebuilt.
		struct StatusInputs
		{
			std::optional<std::string> oAppId;
			bool bOverrideActive = false;
			bool bHasSavedPerGameConfig = false;
			std::vector<std::string> vecProfileNames;
			std::vector<std::string> vecOtherGameIds;
			std::string sLastAppliedProfile;
			bool bHasBackup = false;           // "Restore previous settings" exists only while this is true
			bool bBackupWasOverride = false;   // which area's Restore row applies
		};

		inline uint64_t StatusHash( const StatusInputs &in )
		{
			uint64_t ulHash = 1469598103934665603ull;
			const auto Mix = [ &ulHash ]( std::string_view sv )
			{
				for ( unsigned char c : sv )
				{
					ulHash ^= c;
					ulHash *= 1099511628211ull;
				}
				ulHash ^= 0xff;
				ulHash *= 1099511628211ull;
			};

			Mix( in.oAppId ? *in.oAppId : std::string( "-" ) );
			Mix( in.bOverrideActive ? "override" : "global" );
			Mix( in.bHasSavedPerGameConfig ? "saved" : "nosaved" );
			for ( const std::string &s : in.vecProfileNames )
				Mix( s );
			Mix( "|" );
			for ( const std::string &s : in.vecOtherGameIds )
				Mix( s );
			Mix( "|" );
			Mix( in.sLastAppliedProfile );
			Mix( in.bHasBackup ? ( in.bBackupWasOverride ? "backup-pergame" : "backup-global" ) : "nobackup" );
			return ulHash;
		}

		// The Status rows' wording, in one place so the two areas cannot
		// drift apart and a test can pin it.
		inline std::string PerGameFilePath( const std::optional<std::string> &oAppId )
		{
			return oAppId ? ( "games/" + *oAppId + ".json" ) : std::string( "(no app id)" );
		}

		inline std::string EditsGoTo( const std::optional<std::string> &oAppId, bool bOverrideActive )
		{
			return ( bOverrideActive && oAppId )
				? ( "this game (" + PerGameFilePath( oAppId ) + ")" )
				: std::string( "global.json" );
		}

		inline std::string ProfileFact( const std::string &sLastAppliedProfile )
		{
			return sLastAppliedProfile.empty() ? std::string( "none" ) : sLastAppliedProfile;
		}

		inline std::string GameFact( const std::optional<std::string> &oAppId )
		{
			return oAppId ? ( "app " + *oAppId ) : std::string( "none identified" );
		}

		inline std::string OwnSettingsFact( bool bOverrideActive )
		{
			return bOverrideActive
				? "on -- this game runs on its own settings, loaded whenever it starts"
				: "off -- using the shared global settings";
		}
	}
}
