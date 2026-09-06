// The Setup section's areas of the settings overlay -- Profiles and
// Appearance. See superdoc/features/profiles.md (Profiles v2) and
// superdoc/planning/profiles-concept.md.
//
// The Profiles area is BEING REBUILT (2026-09-06): the file layer for the
// v2 model (Config/ConfigManager.h) is done, the list/modal UI is next.
// PanelConfig.cpp registers a placeholder Status row until then.
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

	// The E2 registration: setup.profiles and setup.appearance (the former
	// setup.pergame area is gone -- a game's settings are a profile now).
	void PanelConfig_RegisterAreas( ui::Registry &reg );

	// =====================================================================
	//  The pure half of the panel -- no ImGui, no disk, no compositor.
	// =====================================================================
	// Header-only so tests/test_overlay_profiles.cpp can hold it to account
	// without linking PanelConfig.cpp.
	namespace panelconfig
	{
		// What the list shows for a profile: general profiles by name, game
		// profiles as "[Game] <display name>" -- the game's title when one
		// has been seen, else its app id, so a migrated profile that has
		// never run yet still reads as a game's.
		inline std::string ListLabel( const config::ProfileMeta &m )
		{
			if ( m.kind != config::ProfileKind::Game )
				return m.name;
			const std::string &sGame = !m.game_name.empty() ? m.game_name
				: ( !m.app_id.empty() ? m.app_id : m.name );
			return "[Game] " + sGame;
		}

		// "Filter Game Profiles": a general profile always shows; a game
		// profile shows when the filter is off, or when it belongs to the
		// session's game.
		inline bool ShowsInList( const config::ProfileMeta &m, bool bFilterOtherGames,
		                         const std::optional<std::string> &oSessionAppId )
		{
			if ( m.kind != config::ProfileKind::Game || !bFilterOtherGames )
				return true;
			return oSessionAppId && m.app_id == *oSessionAppId;
		}

		// The picker index made to agree with what the picker shows: an
		// empty list is -1; anything out of range (either side) becomes 0,
		// the item the picker draws. (2026-09-05 laptop repro: "Delete
		// profile" armed and never deleted, because the index was still -1
		// while the Choice drew item 0.)
		inline int ClampPickerSelection( int nSelected, size_t nCount )
		{
			if ( nCount == 0 )
				return -1;
			if ( nSelected < 0 || nSelected >= (int)nCount )
				return 0;
			return nSelected;
		}

		inline std::string GameFact( const std::optional<std::string> &oAppId )
		{
			return oAppId ? ( "app " + *oAppId ) : std::string( "none identified" );
		}
	}
}
