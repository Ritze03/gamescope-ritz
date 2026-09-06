// The Setup section's areas of the settings overlay -- Profiles and
// Appearance. See superdoc/features/profiles.md (Profiles v2, and the
// "The Profiles area" section for the UI) and
// superdoc/planning/profiles-concept.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Config/ConfigSchema.h"
#include "Config/ConfigManager.h"   // SanitizeProfileName(), for the form check

namespace gamescope
{
	namespace ui { class Registry; }

	// The E2 registration: setup.profiles and setup.appearance (the former
	// setup.pergame area is gone -- a game's settings are a profile now).
	// Also installs the registry-wide session badge and the inheritance
	// seam every other area's rows are marked through (Registry.h's
	// DefaultBadge() / Inheritance()).
	void PanelConfig_RegisterAreas( ui::Registry &reg );

	// =====================================================================
	//  The pure half of the panel -- no ImGui, no disk, no compositor.
	// =====================================================================
	// Header-only so tests/test_overlay_profiles.cpp can hold it to account
	// without linking PanelConfig.cpp. Everything the list, the modals and
	// the status row DECIDE lives here; PanelConfig.cpp only draws and calls
	// the config layer.
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

		// The list's tag / label split of ListLabel(): the tag is drawn
		// muted, the label in the label role.
		inline std::string ListTag( const config::ProfileMeta &m )
		{
			return m.kind == config::ProfileKind::Game ? "[Game]" : "";
		}
		inline std::string ListName( const config::ProfileMeta &m )
		{
			const std::string s = ListLabel( m );
			return m.kind == config::ProfileKind::Game ? s.substr( 7 ) : s;
		}

		// The right-aligned secondary text of one line: "launch option" on
		// the line the `--profile` override selected (it is what the session
		// edits, but not the assignment), else "inherits <parent>" for an
		// inheriting game profile, else nothing.
		inline std::string ListSecondary( const config::ProfileMeta &m, std::string_view svSessionProfile,
		                                  bool bSessionOverride )
		{
			if ( bSessionOverride && m.name == svSessionProfile )
				return "launch option";
			if ( m.kind == config::ProfileKind::Game && !m.inherits.empty() )
				return "inherits " + m.inherits;
			return {};
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

		// The indices (into `all`) the list draws, in `all`'s order. The
		// session profile is ALWAYS shown, filter or not: a `--profile` of
		// another game's profile is what the session is editing, and a list
		// that hid the selected line would have no line to outline.
		inline std::vector<size_t> VisibleProfiles( const std::vector<config::ProfileMeta> &all,
		                                            bool bFilterOtherGames,
		                                            const std::optional<std::string> &oSessionAppId,
		                                            std::string_view svSessionProfile )
		{
			std::vector<size_t> out;
			for ( size_t i = 0; i < all.size(); ++i )
				if ( all[ i ].name == svSessionProfile || ShowsInList( all[ i ], bFilterOtherGames, oSessionAppId ) )
					out.push_back( i );
			return out;
		}

		// Index of `svName` among the visible lines, or -1.
		inline int VisibleIndexOf( const std::vector<config::ProfileMeta> &all,
		                           const std::vector<size_t> &visible, std::string_view svName )
		{
			for ( size_t i = 0; i < visible.size(); ++i )
				if ( all[ visible[ i ] ].name == svName )
					return (int)i;
			return -1;
		}

		// The Inherits dropdown's options: "None", then every general
		// profile in list order. Option i > 0 is names[ i ].
		inline std::vector<std::string> InheritOptionNames( const std::vector<config::ProfileMeta> &all )
		{
			std::vector<std::string> names{ "None" };
			for ( const config::ProfileMeta &m : all )
				if ( m.kind == config::ProfileKind::General )
					names.push_back( m.name );
			return names;
		}

		// Which option a profile's `inherits` is; 0 (None) for "" and for a
		// parent that is not in the list.
		inline int InheritIndex( const std::vector<std::string> &names, std::string_view svInherits )
		{
			for ( size_t i = 1; i < names.size(); ++i )
				if ( names[ i ] == svInherits )
					return (int)i;
			return 0;
		}

		inline size_t CountChildren( const std::vector<config::ProfileMeta> &all, std::string_view svParent )
		{
			size_t n = 0;
			for ( const config::ProfileMeta &m : all )
				if ( m.kind == config::ProfileKind::Game && m.inherits == svParent )
					++n;
			return n;
		}

		// The general profile a NEW game profile inherits from: the one the
		// session is editing when that is general, else the session's own
		// parent. "The base setup, tweaked per game" -- so a game profile
		// created from the current values starts as a clean child of the
		// general profile those values came from, storing nothing until the
		// user changes something.
		inline std::string NewGameInherits( const config::ProfileMeta &session )
		{
			return session.kind == config::ProfileKind::General ? session.name : session.inherits;
		}

		// ---- the Create / Copy / Edit form ----------------------------------
		// One check for all three modals. Errors are per field, so each
		// lands beside the field it is about (the Text atom's own red
		// boundary) with its sentence under the form. `svIgnoreName` is the
		// profile being edited, which may of course keep its own name.
		struct FormCheck
		{
			std::string sName;         // the sanitized name, when sNameError is empty
			std::string sNameError;
			std::string sAppIdError;
			bool ok() const { return sNameError.empty() && sAppIdError.empty(); }
		};

		inline bool AllDigits( std::string_view sv )
		{
			if ( sv.empty() )
				return false;
			for ( char c : sv )
				if ( c < '0' || c > '9' )
					return false;
			return true;
		}

		inline FormCheck CheckProfileForm( bool bGame, std::string_view svAppId, std::string_view svRawName,
		                                   const std::vector<config::ProfileMeta> &existing,
		                                   std::string_view svIgnoreName = {} )
		{
			FormCheck out;
			const std::optional<std::string> oName = config::SanitizeProfileName( svRawName );
			if ( !oName )
				out.sNameError = "Enter a name: letters, digits, spaces, - and _";
			else if ( *oName != svRawName )
				out.sNameError = "Only letters, digits, spaces, - and _ (no leading or trailing spaces)";
			else
			{
				out.sName = *oName;
				for ( const config::ProfileMeta &m : existing )
					if ( m.name == *oName && m.name != svIgnoreName )
						out.sNameError = "A profile named '" + m.name + "' already exists";
			}
			if ( bGame )
			{
				if ( svAppId.empty() )
					out.sAppIdError = "Enter the game's app id";
				else if ( !AllDigits( svAppId ) )
					out.sAppIdError = "The app id is digits only";
			}
			return out;
		}

		// ---- Delete ------------------------------------------------------------
		// Why the last profile cannot go: the session always edits SOME
		// profile, and deleting the only one would recreate `Default` from
		// the built-in defaults behind the user's back -- a silent reset of
		// every setting, which "never delete a config automatically" forbids
		// in spirit.
		inline std::string DeleteBlocker( size_t nProfiles )
		{
			return nProfiles <= 1 ? "this is the last profile; create another before deleting it" : "";
		}

		inline std::string DeleteChildrenLine( size_t nChildren )
		{
			if ( nChildren == 0 )
				return {};
			if ( nChildren == 1 )
				return "1 profile inherits from it and will keep its values.";
			return std::to_string( nChildren ) + " profiles inherit from it and will keep its values.";
		}

		// ---- the status row and the badge ---------------------------------------
		inline std::string GameStatusFact( std::string_view svGameName, const std::optional<std::string> &oAppId )
		{
			if ( !oAppId )
				return "none identified";
			if ( svGameName.empty() || svGameName == *oAppId )
				return *oAppId;
			return std::string( svGameName ) + " (" + *oAppId + ")";
		}

		// The Status row's summary, in the sheet's control zone:
		// `[Game] Rust · inherits Comp · game Rust`, or `Casual (launch) ·
		// game Rust` while `--profile` is in force. Kept to the essentials
		// because the control zone beside a column Inspector is ~40
		// characters wide and DrawText clips the tail; the app id and the
		// long form (`editing: ... · game: Rust (252490) · launch option:
		// Casual (this session)`) are the row's Live facts, one per line.
		inline std::string StatusSummary( const config::ProfileMeta &session, std::string_view svGameName,
		                                  const std::optional<std::string> &oAppId,
		                                  const std::optional<std::string> &oOverride )
		{
			std::string s = oOverride ? *oOverride + " (launch)" : ListLabel( session );
			if ( !oOverride && session.kind == config::ProfileKind::Game && !session.inherits.empty() )
				s += " · inherits " + session.inherits;
			// The game, only when the profile's own label does not already
			// say it: "[Game] Rust" under Rust is the common case and
			// repeating the name there is the difference between fitting
			// the control zone and clipping.
			const bool bLabelSaysGame = !oOverride && session.kind == config::ProfileKind::Game &&
				oAppId && session.app_id == *oAppId;
			if ( !oAppId )
				s += " · no game identified";
			else if ( !bLabelSaysGame )
				s += " · game " + ( svGameName.empty() ? *oAppId : std::string( svGameName ) );
			return s;
		}

		// The same, spelled out, for the Inspector's LIVE facts.
		inline std::string StatusLong( const config::ProfileMeta &session, std::string_view svGameName,
		                               const std::optional<std::string> &oAppId,
		                               const std::optional<std::string> &oOverride )
		{
			std::string s = "editing: " + ListLabel( session );
			if ( session.kind == config::ProfileKind::Game && !session.inherits.empty() )
				s += " · inherits " + session.inherits;
			s += " · game: " + GameStatusFact( svGameName, oAppId );
			if ( oOverride )
				s += " · launch option: " + *oOverride + " (this session)";
			return s;
		}

		// Every area's badge: the session profile as the list labels it, or
		// `Casual (launch)` while a `--profile` override is in force.
		inline std::string SessionBadge( const config::ProfileMeta &session, bool bOverride )
		{
			return bOverride ? session.name + " (launch)" : ListLabel( session );
		}
	}
}
