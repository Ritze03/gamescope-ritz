// The Setup section's areas of the settings overlay -- Profiles and
// Appearance. See superdoc/features/profiles.md (Profiles v2, and the
// "The Profiles area" section for the UI) and
// superdoc/planning/profiles-concept.md.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Config/ConfigSchema.h"
#include "Config/ConfigManager.h"   // SanitizeProfileName(), for the form check
#include "Config/AppId.h"           // SanitizeAppId(), for the form check
#include "Config/ProfileExport.h"   // the Export / Import dialogs
#include "UI/Controls.h"            // CheckState, ListBoxStep(): the dialogs' tree

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
		// What the list, the badge, the Status row and the modal titles call
		// a profile: general profiles by name, game profiles as
		// "[Game] <profile name>" -- ALWAYS the profile's own name, never
		// the game's. (Until 2026-09-06 a game profile was labelled with the
		// game's window title; the user: "It shows the process/game name.
		// Not the actual profile name." A profile named CS2 for the game
		// "Counter-Strike 2" must read as CS2 everywhere, because that is
		// the name the user typed, the name in the toast, and the file.)
		// The game's name is the LINE's secondary text -- GameName() below.
		inline std::string ListLabel( const config::ProfileMeta &m )
		{
			return m.kind == config::ProfileKind::Game ? "[Game] " + m.name : m.name;
		}

		// The list's tag / label split of ListLabel(): the tag is drawn
		// muted, the label in the label role.
		inline std::string ListTag( const config::ProfileMeta &m )
		{
			return m.kind == config::ProfileKind::Game ? "[Game]" : "";
		}
		inline std::string ListName( const config::ProfileMeta &m )
		{
			return m.name;
		}

		// The game a game profile belongs to, for the muted text: the title
		// seen for it (`game_name`), else its app id, so a migrated profile
		// that has never run yet still says which game it is.
		inline std::string GameName( const config::ProfileMeta &m )
		{
			if ( m.kind != config::ProfileKind::Game )
				return {};
			return !m.game_name.empty() ? m.game_name : m.app_id;
		}

		// The right-aligned, muted secondary text of one line:
		// "launch option" on the line the `--profile` override selected (it
		// is what the session edits, but not the assignment); the game's
		// name on a game profile's line, then "inherits <parent>" when it
		// inherits -- `Counter-Strike 2 · inherits Comp`, or the game alone.
		inline std::string ListSecondary( const config::ProfileMeta &m, std::string_view svSessionProfile,
		                                  bool bSessionOverride )
		{
			std::string s;
			auto Add = [ &s ]( const std::string &sPart )
			{
				if ( sPart.empty() )
					return;
				if ( !s.empty() )
					s += " · ";
				s += sPart;
			};
			if ( bSessionOverride && m.name == svSessionProfile )
				Add( "launch option" );
			Add( GameName( m ) );
			if ( m.kind == config::ProfileKind::Game && !m.inherits.empty() )
				Add( "inherits " + m.inherits );
			return s;
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
			std::string sAppId;        // the sanitized (trimmed) app id, when sAppIdError is empty
			std::string sAppIdError;
			bool ok() const { return sNameError.empty() && sAppIdError.empty(); }
		};

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
				// App ids are opaque strings now, not digits-only (2026-09-15,
				// "Make it fully compatible with string based IDs ... for
				// setting the ID manually") -- Steam app ids happen to be
				// numeric, but a manually-set or non-Steam id is any string
				// SanitizeAppId() (Config/AppId.h) accepts. Distinguish "blank"
				// from "has a rejected character" up front, since
				// SanitizeAppId() collapses both to nullopt.
				const bool bBlank = svAppId.find_first_not_of( " \t\r\n" ) == std::string_view::npos;
				if ( bBlank )
					out.sAppIdError = "Enter the game's app id";
				else if ( const std::optional<std::string> oAppId = config::SanitizeAppId( svAppId ) )
					out.sAppId = *oAppId;
				else
					out.sAppIdError = "The app id can't contain '/', '\\', or control characters";
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
		// `[Game] Rust · inherits Comp`, `Casual · game Rust`, or `Casual
		// (launch) · game Rust` while `--profile` is in force. Kept to the
		// essentials because the control zone beside a column Inspector is
		// ~40 characters wide and DrawText clips the tail; the app id and
		// the long form (`editing: ... · game: Rust (252490) · launch
		// option: Casual (this session)`) are the row's Live facts, one per
		// line.
		inline std::string StatusSummary( const config::ProfileMeta &session, std::string_view svGameName,
		                                  const std::optional<std::string> &oAppId,
		                                  const std::optional<std::string> &oOverride )
		{
			std::string s = oOverride ? *oOverride + " (launch)" : ListLabel( session );
			if ( !oOverride && session.kind == config::ProfileKind::Game && !session.inherits.empty() )
				s += " · inherits " + session.inherits;
			// The game, only when the profile is not this game's own: a
			// game profile bound to the running game is the common case,
			// and naming the game there is the difference between fitting
			// the control zone and clipping (the Live facts carry it).
			const bool bOwnGameProfile = !oOverride && session.kind == config::ProfileKind::Game &&
				oAppId && session.app_id == *oAppId;
			if ( !oAppId )
				s += " · no game identified";
			else if ( !bOwnGameProfile )
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

		// =================================================================
		//  Export / Import dialogs (2026-11-07) -- settings as JSON TEXT
		//  through the clipboard. superdoc/features/profile-export.md.
		//  Every decision of the two dialogs is a plain function here; the
		//  dialogs in PanelConfig.cpp only draw and call ProfileExport.
		// =================================================================
		using ExportGroups = std::vector<config::profile_export::ExportGroup>;
		using GroupSet     = config::profile_export::GroupSet;

		// ---- the selection tree (the same tree in both dialogs) ---------
		// One flat list of rows: a parent (the rail group's label) followed
		// by its children (the groups under it), in the groups' own order.
		struct TreeRow
		{
			bool        bParent = false;
			int         nGroup  = -1;     // index into the groups; -1 for a parent
			std::string sParent;          // the parent label this row is, or sits under
		};

		inline std::vector<TreeRow> TreeRows( const ExportGroups &groups )
		{
			std::vector<TreeRow> out;
			std::vector<std::string> parents;
			for ( const auto &g : groups )
				if ( std::find( parents.begin(), parents.end(), g.parent ) == parents.end() )
					parents.push_back( g.parent );
			for ( const std::string &sParent : parents )
			{
				out.push_back( TreeRow{ true, -1, sParent } );
				for ( size_t i = 0; i < groups.size(); ++i )
					if ( groups[ i ].parent == sParent )
						out.push_back( TreeRow{ false, (int)i, sParent } );
			}
			return out;
		}

		// `pUsable` null = every group can be ticked (Export). A group outside
		// it (Import: not in the pasted text) is greyed, never ticked, and a
		// parent counts and changes only its usable children.
		inline bool TreeUsable( const GroupSet *pUsable, const std::string &sId )
		{
			return !pUsable || pUsable->count( sId ) != 0;
		}

		inline size_t TreeUsableChildren( const ExportGroups &groups, const std::string &sParent,
		                                  const GroupSet *pUsable )
		{
			size_t n = 0;
			for ( const auto &g : groups )
				if ( g.parent == sParent && TreeUsable( pUsable, g.id ) )
					++n;
			return n;
		}

		inline ui::controls::CheckState TreeParentState( const ExportGroups &groups, const std::string &sParent,
		                                                const GroupSet &ticked, const GroupSet *pUsable )
		{
			size_t nOn = 0, nTotal = 0;
			for ( const auto &g : groups )
			{
				if ( g.parent != sParent || !TreeUsable( pUsable, g.id ) )
					continue;
				++nTotal;
				nOn += ticked.count( g.id ) ? 1 : 0;
			}
			return ui::controls::CheckAggregate( nOn, nTotal );
		}

		// A click on a parent sets every usable child: Off and Mixed turn them
		// all on, On turns them all off (CheckNext()).
		inline void TreeClickParent( const ExportGroups &groups, const std::string &sParent,
		                             GroupSet &ticked, const GroupSet *pUsable )
		{
			const bool bOn = ui::controls::CheckNext( TreeParentState( groups, sParent, ticked, pUsable ) ) ==
				ui::controls::CheckState::On;
			for ( const auto &g : groups )
			{
				if ( g.parent != sParent || !TreeUsable( pUsable, g.id ) )
					continue;
				if ( bOn )
					ticked.insert( g.id );
				else
					ticked.erase( g.id );
			}
		}

		inline void TreeClickChild( GroupSet &ticked, const std::string &sId, const GroupSet *pUsable )
		{
			if ( !TreeUsable( pUsable, sId ) )
				return;
			if ( !ticked.erase( sId ) )
				ticked.insert( sId );
		}

		inline ui::controls::CheckState TreeChildState( const GroupSet &ticked, const std::string &sId )
		{
			return ticked.count( sId ) ? ui::controls::CheckState::On : ui::controls::CheckState::Off;
		}

		// Which rows the keyboard cursor may stop on and Space may toggle.
		inline std::vector<bool> TreeRowEnabled( const ExportGroups &groups, const std::vector<TreeRow> &rows,
		                                         const GroupSet *pUsable )
		{
			std::vector<bool> out;
			for ( const TreeRow &r : rows )
				out.push_back( r.bParent ? TreeUsableChildren( groups, r.sParent, pUsable ) > 0
				                         : TreeUsable( pUsable, groups[ (size_t)r.nGroup ].id ) );
			return out;
		}

		// One cursor step over the rows, skipping greyed ones. -1 = no cursor
		// yet: the first arrow lands on the first usable row (ListBoxStep's
		// own rule). At either end, or with nothing usable, the cursor stays.
		inline int TreeCursorStep( const std::vector<bool> &enabled, int nCursor, ui::controls::ListBoxNav eNav )
		{
			const int nCount = (int)enabled.size();
			if ( std::find( enabled.begin(), enabled.end(), true ) == enabled.end() )
				return -1;
			using Nav = ui::controls::ListBoxNav;
			int nCur = nCursor;
			Nav eStep = eNav;
			if ( nCur < 0 )
			{
				nCur = -1;
				eStep = Nav::Down;
			}
			else if ( eNav == Nav::Home )
			{
				nCur  = ui::controls::ListBoxStep( nCur, nCount, Nav::Home );
				eStep = Nav::Down;
				if ( enabled[ (size_t)nCur ] )
					return nCur;
			}
			else if ( eNav == Nav::End )
			{
				nCur  = ui::controls::ListBoxStep( nCur, nCount, Nav::End );
				eStep = Nav::Up;
				if ( enabled[ (size_t)nCur ] )
					return nCur;
			}
			for ( int i = 0; i <= nCount; ++i )
			{
				const int nNext = ui::controls::ListBoxStep( nCur, nCount, eStep );
				if ( nNext == nCur )
					break;
				nCur = nNext;
				if ( enabled[ (size_t)nCur ] )
					return nCur;
			}
			return nCursor;
		}

		// ---- Export ---------------------------------------------------------
		inline bool ExportPrimaryEnabled( const GroupSet &ticked )
		{
			return !ticked.empty();
		}

		// "Settings copied: 3 groups". Counts the ticked groups.
		inline std::string ExportToast( size_t nGroups )
		{
			return "Settings copied: " + std::to_string( nGroups ) + ( nGroups == 1 ? " group" : " groups" );
		}

		// Only an inheriting game profile has the "Include inherited values"
		// option; any other profile is always exported in full.
		inline bool ExportOffersInherited( const config::ProfileMeta &m )
		{
			return m.kind == config::ProfileKind::Game && !m.inherits.empty();
		}

		// ---- Import ---------------------------------------------------------
		// What the paste field holds: nothing yet (no message, no error), or
		// something to parse.
		inline bool ImportTextBlank( const std::string &sText )
		{
			return sText.find_first_not_of( " \t\r\n" ) == std::string::npos;
		}

		// Only groups the pasted text has can be ticked.
		inline GroupSet ImportUsable( const std::vector<std::string> &present )
		{
			return GroupSet( present.begin(), present.end() );
		}

		// The starting ticks of a fresh paste: every present group that is
		// ticked by default (Resolution is not: it depends on the machine).
		inline GroupSet ImportInitialTicks( const ExportGroups &groups, const std::vector<std::string> &present )
		{
			const GroupSet usable = ImportUsable( present );
			GroupSet out;
			for ( const auto &g : groups )
				if ( g.default_ticked && usable.count( g.id ) )
					out.insert( g.id );
			return out;
		}

		// The ticks that count: never more than what the paste has.
		inline GroupSet TicksWithin( const GroupSet &ticked, const GroupSet &usable )
		{
			GroupSet out;
			for ( const std::string &s : ticked )
				if ( usable.count( s ) )
					out.insert( s );
			return out;
		}

		// The name a new profile starts with: the pasted profile's own name,
		// else "Imported", made free with " 2", " 3" ... (the same rule
		// ApplyImport applies when the name is taken).
		inline std::string ImportSuggestedName( std::string_view svPastedProfile,
		                                        const std::vector<config::ProfileMeta> &existing )
		{
			std::string sBase = config::SanitizeProfileName( svPastedProfile ).value_or( "" );
			if ( sBase.empty() )
				sBase = "Imported";
			auto Taken = [ &existing ]( const std::string &s )
			{
				for ( const config::ProfileMeta &m : existing )
					if ( m.name == s )
						return true;
				return false;
			};
			if ( !Taken( sBase ) )
				return sBase;
			for ( int n = 2; n < 10000; ++n )
				if ( !Taken( sBase + " " + std::to_string( n ) ) )
					return sBase + " " + std::to_string( n );
			return sBase;
		}

		// The new-profile name field's refusal, "" when fine: the Create form's
		// own rule (CheckProfileForm), so the two cannot disagree.
		inline std::string ImportNameError( const std::string &sName,
		                                    const std::vector<config::ProfileMeta> &existing )
		{
			return CheckProfileForm( false, {}, sName, existing ).sNameError;
		}

		// Import is available only for valid JSON with a ticked group, and for
		// a new profile with a usable name. Nothing else reaches ApplyImport.
		inline bool ImportPrimaryEnabled( bool bValidParse, const GroupSet &effectiveTicks, bool bNewProfile,
		                                  bool bNameOk )
		{
			return bValidParse && !effectiveTicks.empty() && ( !bNewProfile || bNameOk );
		}

		inline std::string ImportBackupNote( const std::string &sDest )
		{
			return "A backup of " + sDest + " is kept as " + sDest + ".json.bak";
		}

		inline std::string UnknownLine( int nUnknown )
		{
			return std::to_string( nUnknown ) + ( nUnknown == 1 ? " unknown value skipped" : " unknown values skipped" );
		}

		inline std::string UnreadableLine( size_t nSkipped )
		{
			return std::to_string( nSkipped ) +
				( nSkipped == 1 ? " value cannot be read and stays as it is" : " values cannot be read and stay as they are" );
		}

		// The Changes pane's headline.
		inline std::string ChangesSummary( bool bNewProfile, const std::string &sName, int nCount )
		{
			const std::string sN = std::to_string( nCount ) + ( nCount == 1 ? " setting" : " settings" );
			if ( bNewProfile )
				return "New profile '" + sName + "': " + sN + " set";
			if ( nCount == 0 )
				return "No settings change";
			return sN + ( nCount == 1 ? " changes" : " change" );
		}

		// "Imported into 'Comp': 12 settings, 2 skipped". A new profile reads
		// "Imported as". `nSkipped` is everything pasted that was not applied
		// (unreadable values and keys this build does not know).
		inline std::string ImportToast( const std::string &sDest, bool bNewProfile, int nApplied, int nSkipped )
		{
			std::string s;
			if ( nApplied == 0 && !bNewProfile )
				s = "Nothing changed in '" + sDest + "'";
			else
				s = std::string( bNewProfile ? "Imported as '" : "Imported into '" ) + sDest + "': " +
					std::to_string( nApplied ) + ( nApplied == 1 ? " setting" : " settings" );
			if ( nSkipped > 0 )
				s += ", " + std::to_string( nSkipped ) + " skipped";
			return s;
		}

		// "Imported into 3 profiles: 45 settings, 2 skipped" (several destinations;
		// one destination keeps ImportToast's wording).
		inline std::string ImportToastMulti( size_t nProfiles, int nApplied, int nSkipped )
		{
			std::string s = "Imported into " + std::to_string( nProfiles ) + " profiles: " +
				std::to_string( nApplied ) + ( nApplied == 1 ? " setting" : " settings" );
			if ( nSkipped > 0 )
				s += ", " + std::to_string( nSkipped ) + " skipped";
			return s;
		}

		// Under the destination list: "Imports into 'X'" or "Imports into 3 profiles".
		inline std::string ImportTargetLine( const std::vector<std::string> &names )
		{
			if ( names.size() == 1 )
				return "Imports into '" + names[ 0 ] + "'";
			return "Imports into " + std::to_string( names.size() ) + " profiles";
		}

		// The Source pane's message for a profile picked as the source.
		inline std::string ImportSourceMessage( const std::string &sName, size_t nGroups )
		{
			return "Settings of '" + sName + "': " + std::to_string( nGroups ) + ( nGroups == 1 ? " group" : " groups" );
		}

		// A refused press over several destinations: which one, how many were
		// already written. One destination keeps the bare reason.
		inline std::string ImportFailure( size_t nTotal, size_t nWritten, const std::string &sName,
		                                  const std::string &sReason )
		{
			if ( nTotal <= 1 )
				return sReason;
			std::string s = "Could not import into '" + sName + "': " + sReason;
			if ( nWritten == 0 )
				s += " Nothing was changed.";
			else
				s += " " + std::to_string( nWritten ) + ( nWritten == 1 ? " profile was" : " profiles were" ) +
					" already written.";
			return s;
		}

		// The press over N destinations: check ALL first (fnCheck returns the
		// refusal or ""), write none unless every check passes, then write each
		// (fnApply returns the refusal or ""), stopping at the first failure.
		struct MultiApplyOutcome
		{
			bool        ok = true;
			size_t      nWritten = 0;
			size_t      nFailedAt = 0;   // index of the destination that refused
			std::string error;           // its reason
		};
		template <typename FnCheck, typename FnApply>
		inline MultiApplyOutcome ApplyToAll( size_t nDests, FnCheck &&fnCheck, FnApply &&fnApply )
		{
			MultiApplyOutcome o;
			for ( size_t n = 0; n < nDests; ++n )
			{
				std::string sErr = fnCheck( n );
				if ( !sErr.empty() )
				{
					o.ok = false; o.nFailedAt = n; o.error = std::move( sErr );
					return o;
				}
			}
			for ( size_t n = 0; n < nDests; ++n )
			{
				std::string sErr = fnApply( n );
				if ( !sErr.empty() )
				{
					o.ok = false; o.nFailedAt = n; o.error = std::move( sErr );
					return o;
				}
				++o.nWritten;
			}
			return o;
		}

		// ---- Changes lines ---------------------------------------------------
		// A row's label: what the lookup (the registry) calls its key, else the
		// key itself.
		template <typename Fn>
		inline std::string DiffLabel( const std::string &sKey, Fn &&fnLookup )
		{
			const std::string s = fnLookup( sKey );
			return s.empty() ? sKey : s;
		}

		// A value cut for one line; the atlas is Latin-1, so only plain ASCII
		// is added ("...").
		inline std::string ShortValue( const std::string &sJson, size_t nMax = 28 )
		{
			if ( sJson.size() <= nMax || nMax < 4 )
				return sJson;
			return sJson.substr( 0, nMax - 3 ) + "...";
		}

		// "Frame generation / Target fps: 120 -> 240" -- plain ASCII arrow.
		inline std::string DiffLine( const config::profile_export::DiffRow &r, const std::string &sLabel )
		{
			return r.group + " / " + sLabel + ": " + ShortValue( r.old_value ) + " -> " + ShortValue( r.new_value );
		}

		// Greedy word wrap for a monospace column count: a word longer than a
		// line is split. Never returns an empty list (an empty string is one
		// empty line). `sIndent` prefixes every line after the first.
		inline std::vector<std::string> WrapWords( const std::string &s, size_t nCols, std::string sIndent = {} )
		{
			std::vector<std::string> out;
			if ( nCols < 8 )
				nCols = 8;
			if ( sIndent.size() + 1 > nCols )
				sIndent.clear();
			std::string sCur;
			auto Avail = [ & ] { return nCols - ( out.empty() ? 0 : sIndent.size() ); };
			auto Emit = [ & ]
			{
				out.push_back( ( out.empty() ? std::string() : sIndent ) + sCur );
				sCur.clear();
			};
			size_t i = 0;
			while ( i < s.size() )
			{
				if ( s[ i ] == ' ' )
				{
					++i;
					continue;
				}
				size_t j = i;
				while ( j < s.size() && s[ j ] != ' ' )
					++j;
				std::string sWord = s.substr( i, j - i );
				i = j;
				while ( sWord.size() > Avail() )
				{
					if ( !sCur.empty() )
						Emit();
					const size_t nTake = Avail();
					sCur = sWord.substr( 0, nTake );
					sWord.erase( 0, nTake );
					Emit();
				}
				if ( !sCur.empty() && sCur.size() + 1 + sWord.size() > Avail() )
					Emit();
				if ( !sCur.empty() )
					sCur += ' ';
				sCur += sWord;
			}
			if ( !sCur.empty() || out.empty() )
				Emit();
			return out;
		}
	}
}
