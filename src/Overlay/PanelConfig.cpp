// M7 Config panel -- see PanelConfig.h and superdoc/planning/SPEC.md's
// Feature 6 ("Config system") and "UI structure" -> "Config/Profiles panel".
//
// Thread safety: drawn from SettingsOverlay_AddLayer() on the steamcompmgr
// thread, same as PanelDisplay.cpp/PanelShaders.cpp -- see PanelDisplay.cpp's
// file-level comment for the full argument. This panel's own state
// (s_oAppId, s_bOverrideActive, the cached list boxes) is plain, unlocked
// data for the same reason: single-thread-only access.
//
// This panel never keeps its own copy of gamescope/fps_display/reshade
// settings the way PanelDisplay/PanelShaders/FpsDisplay do -- "both global
// and per-game config are editable" (the task's own requirement) is already
// satisfied by those three panels: whichever file
// config::IsSessionOverrideActive() currently names is the one they read
// from and write back to (config::ResolveEffective() / EnqueueRoutedWrite(),
// see ConfigManager.h's session-routing section). This panel's whole job is
// choosing *which* file that is (the override checkbox), and bulk-loading a
// profile or another game's config into it -- never hand-editing individual
// sliders itself, which would just duplicate those three panels' widgets.
#include "PanelConfig.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "Config/ConfigManager.h"
#include "Chrome.h"

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		bool s_bInitialized = false;

		std::optional<std::string> s_oAppId;
		bool s_bOverrideActive = false;

		std::vector<std::string> s_ProfileNames;
		// games/<AppId>.json entries other than the current session's own --
		// "copy another game's config" only ever makes sense as copying
		// *someone else's* settings in.
		std::vector<std::string> s_OtherGameIds;

		int s_nSelectedProfile = -1;
		int s_nSelectedCopyGame = -1;
		char s_szNewProfileName[ 64 ] = {};

		// Transient one-line feedback for the last action taken in this
		// panel (e.g. "Applied profile 'FPS'") -- ponytail: a single static
		// string is enough for a UI where actions are user-initiated button
		// clicks, not a toast/notification queue.
		std::string s_sStatus;

		void RefreshLists()
		{
			s_ProfileNames = config::ListProfiles();
			s_OtherGameIds.clear();
			for ( std::string &sId : config::ListGameIds() )
			{
				if ( !s_oAppId.has_value() || sId != *s_oAppId )
					s_OtherGameIds.push_back( std::move( sId ) );
			}

			if ( s_nSelectedProfile >= (int)s_ProfileNames.size() )
				s_nSelectedProfile = s_ProfileNames.empty() ? -1 : 0;
			if ( s_nSelectedCopyGame >= (int)s_OtherGameIds.size() )
				s_nSelectedCopyGame = s_OtherGameIds.empty() ? -1 : 0;
		}

		void EnsureInitialized()
		{
			if ( s_bInitialized )
				return;
			s_oAppId = config::SessionAppId();
			s_bOverrideActive = config::IsSessionOverrideActive();
			RefreshLists();
			s_bInitialized = true;
		}

		// ---- actions -----------------------------------------------------
		// Every action here ends by bumping the shared config generation
		// (Config/ConfigManager.h) so PanelDisplay/PanelShaders/FpsDisplay's
		// own EnsureConfigLoaded() picks up the change on their very next
		// draw, without needing a restart -- see those files' M7 comments.

		// "Override Global Config" ON: captures a FULL SNAPSHOT of whatever
		// is currently effective (global.json, since override was off) into
		// games/<AppId>.json (DECISIONS.md #19) -- not a sparse delta, and
		// later global.json edits will not reach this game from this point
		// on.
		void EnableOverride()
		{
			if ( !s_oAppId )
				return;
			config::Settings snapshot = config::ResolveEffective( s_oAppId );
			config::EnqueuePerGameSnapshot( *s_oAppId, snapshot );
			config::SetSessionOverrideActive( true );
			config::BumpConfigGeneration();
			s_bOverrideActive = true;
			s_sStatus = "Override enabled -- current settings snapshotted for this game.";
			RefreshLists();
		}

		// "Override Global Config" OFF: deletes games/<AppId>.json outright
		// (ConfigManager.h's ClearPerGameOverride comment: "turn the
		// override back off") so this game falls back to reading
		// global.json again, per Feature 6's strictly-two-level resolution
		// (never a merge of both).
		//
		// ponytail: called inline (not via an Enqueue* background-thread
		// path) -- ClearPerGameOverride() is a bare std::filesystem::remove
		// (a single unlink(), no fsync()/rename()), and this only runs once
		// per checkbox click, not once per slider tick the way
		// EnqueueRoutedWrite() below does. The Enqueue* family exists for
		// the fsync()+rename() write paths that can be hit continuously by
		// a slider drag; a one-off unlink from a button click doesn't need
		// the same treatment.
		void DisableOverride()
		{
			if ( !s_oAppId )
				return;
			config::ClearPerGameOverride( *s_oAppId );
			config::SetSessionOverrideActive( false );
			config::BumpConfigGeneration();
			s_bOverrideActive = false;
			s_sStatus = "Override disabled -- this game's config file was removed; back to global.";
			RefreshLists();
		}

		// Copies values from a profile into whichever file is currently
		// authoritative (the per-game snapshot if override is active, else
		// global.json) -- a ONE-TIME COPY (DECISIONS.md #20): the target
		// has no memory of which profile it came from, and later edits to
		// the profile do not retroactively affect it.
		void ApplySelectedProfile()
		{
			if ( s_nSelectedProfile < 0 || s_nSelectedProfile >= (int)s_ProfileNames.size() )
				return;
			const std::string &sName = s_ProfileNames[ s_nSelectedProfile ];

			config::Settings target = ( s_bOverrideActive && s_oAppId )
				? config::ResolveEffective( s_oAppId )
				: config::LoadGlobal();

			if ( !config::ApplyProfile( target, sName ) )
			{
				s_sStatus = "Failed to apply profile '" + sName + "'.";
				return;
			}

			config::EnqueueRoutedWrite( target );
			config::BumpConfigGeneration();
			s_sStatus = "Applied profile '" + sName + "'.";
		}

		// Saves whichever config is currently in effect (per-game if
		// override is active, else global) as a brand new named profile --
		// SanitizeProfileName() (reused from M0) is what keeps user text
		// input from ever escaping profiles/ as a path.
		void SaveCurrentAsNewProfile()
		{
			std::optional<std::string> oSanitized = config::SanitizeProfileName( s_szNewProfileName );
			if ( !oSanitized )
			{
				s_sStatus = "Profile name can't be empty (letters/digits/space/-/_ only).";
				return;
			}

			config::Settings current = ( s_bOverrideActive && s_oAppId )
				? config::ResolveEffective( s_oAppId )
				: config::LoadGlobal();

			config::EnqueueProfileWrite( *oSanitized, current );
			s_sStatus = "Saved profile '" + *oSanitized + "'.";
			s_szNewProfileName[ 0 ] = '\0';
			RefreshLists();
		}

		// Copies another game's *entire* config into this game's own
		// per-game file -- same full-snapshot, one-time-copy semantics as
		// EnableOverride()/ApplySelectedProfile() above, and implicitly
		// turns "Override Global Config" on for this game too (there's no
		// other file for a per-game copy to land in).
		void CopySelectedGameConfig()
		{
			if ( !s_oAppId || s_nSelectedCopyGame < 0 || s_nSelectedCopyGame >= (int)s_OtherGameIds.size() )
				return;
			const std::string &sSourceId = s_OtherGameIds[ s_nSelectedCopyGame ];

			// Every file ListGameIds() can see was, by construction, only
			// ever created with override_global: true (SnapshotPerGameOverride
			// is the sole writer of games/<AppId>.json) -- LoadPerGameOverride()
			// re-validates that flag anyway rather than trusting the listing.
			std::optional<config::Settings> oSource = config::LoadPerGameOverride( sSourceId );
			if ( !oSource )
			{
				s_sStatus = "Couldn't read app " + sSourceId + "'s config.";
				return;
			}

			config::EnqueuePerGameSnapshot( *s_oAppId, *oSource );
			config::SetSessionOverrideActive( true );
			config::BumpConfigGeneration();
			s_bOverrideActive = true;
			s_sStatus = "Copied app " + sSourceId + "'s config to this game.";
			RefreshLists();
		}

		// ---- drawing -------------------------------------------------------

		void DrawPerGameTab()
		{
			if ( !s_oAppId )
			{
				// No app id resolved for this session at all (standalone
				// gamescope-ritz run, no GS_RITZ_APPID/Steam env vars) --
				// SPEC.md Feature 6: degrade honestly rather than offering a
				// per-game editor with nothing to write to. games/<AppId>.json
				// is never looked up or created in this state.
				ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ), "No game identified for this session." );
				ImGui::TextWrapped(
					"gamescope-ritz wasn't launched with GS_RITZ_APPID or a Steam app-id "
					"environment variable set, so there's no per-game config file to edit. "
					"All changes made in the other panels are going to global.json." );
				ImGui::Separator();
				ImGui::TextUnformatted( "Profiles still apply to the global config below." );
			}
			else
			{
				ImGui::Text( "Game app id: %s", s_oAppId->c_str() );
				ImGui::TextUnformatted( s_bOverrideActive
					? "Editing: this game's own config (games/<id>.json) -- frozen snapshot, independent of global."
					: "Editing: global config (global.json) -- shared with every other game that doesn't override it." );

				bool bOverride = s_bOverrideActive;
				if ( ImGui::Checkbox( "Override Global Config", &bOverride ) )
				{
					if ( bOverride )
						EnableOverride();
					else
						DisableOverride();
				}
				ImGui::TextDisabled( "Takes a full snapshot of the current settings; later global changes won't reach this game." );
			}

			ImGui::Separator();
			ImGui::TextUnformatted( "Profiles" );

			if ( s_ProfileNames.empty() )
			{
				ImGui::TextDisabled( "No profiles saved yet." );
			}
			else
			{
				const char *pszPreview = ( s_nSelectedProfile >= 0 && s_nSelectedProfile < (int)s_ProfileNames.size() )
					? s_ProfileNames[ s_nSelectedProfile ].c_str() : "";
				if ( ImGui::BeginCombo( "##ProfilePicker", pszPreview ) )
				{
					for ( int i = 0; i < (int)s_ProfileNames.size(); i++ )
					{
						const bool bSelected = ( i == s_nSelectedProfile );
						if ( ImGui::Selectable( s_ProfileNames[ i ].c_str(), bSelected ) )
							s_nSelectedProfile = i;
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				ImGui::BeginDisabled( s_nSelectedProfile < 0 );
				if ( ImGui::Button( "Apply##Profile" ) )
					ApplySelectedProfile();
				ImGui::EndDisabled();
				ImGui::TextDisabled( "Copies the profile's values in once; editing the profile later won't retroactively change this." );
			}

			ImGui::InputTextWithHint( "##NewProfileName", "New profile name", s_szNewProfileName, sizeof( s_szNewProfileName ) );
			ImGui::SameLine();
			if ( ImGui::Button( "Save as new profile" ) )
				SaveCurrentAsNewProfile();

			if ( s_oAppId )
			{
				ImGui::Separator();
				ImGui::TextUnformatted( "Copy another game's config" );
				if ( s_OtherGameIds.empty() )
				{
					ImGui::TextDisabled( "No other game has an overridden config yet." );
				}
				else
				{
					const char *pszPreview = ( s_nSelectedCopyGame >= 0 && s_nSelectedCopyGame < (int)s_OtherGameIds.size() )
						? s_OtherGameIds[ s_nSelectedCopyGame ].c_str() : "";
					if ( ImGui::BeginCombo( "##CopyGamePicker", pszPreview ) )
					{
						for ( int i = 0; i < (int)s_OtherGameIds.size(); i++ )
						{
							const bool bSelected = ( i == s_nSelectedCopyGame );
							if ( ImGui::Selectable( s_OtherGameIds[ i ].c_str(), bSelected ) )
								s_nSelectedCopyGame = i;
						}
						ImGui::EndCombo();
					}
					ImGui::SameLine();
					ImGui::BeginDisabled( s_nSelectedCopyGame < 0 );
					if ( ImGui::Button( "Copy##Game" ) )
						CopySelectedGameConfig();
					ImGui::EndDisabled();
					ImGui::TextDisabled( "Also turns Override Global Config on for this game." );
				}
			}

			if ( !s_sStatus.empty() )
			{
				ImGui::Separator();
				ImGui::TextColored( ImVec4( 0.6f, 0.85f, 0.6f, 1.0f ), "%s", s_sStatus.c_str() );
			}
		}

		// General settings for gamescope-ritz itself. Deliberately thin:
		// the task brief's only named General-section setting is the
		// Lossless.dll path, and that's explicitly out of scope (LSFG-VK
		// was cut from this roadmap, DECISIONS.md #1); config-system.md's
		// own open question 5 turned up nothing else in the source pass
		// either. Rather than invent settings the spec doesn't ask for,
		// this tab is an honest placeholder plus one genuinely useful piece
		// of read-only info (where the config files actually live, for
		// anyone hand-editing/backing them up).
		void DrawGeneralTab()
		{
			ImGui::TextUnformatted( "gamescope-ritz" );
			ImGui::Separator();
			ImGui::TextUnformatted( "Config directory:" );
			ImGui::TextWrapped( "%s", config::ConfigRoot().c_str() );
			ImGui::Spacing();
			ImGui::TextDisabled(
				"Lossless.dll path (LSFG-VK) is out of scope for this roadmap -- "
				"LSFG-VK integration was cut (see DECISIONS.md #1)." );
			ImGui::TextDisabled(
				"No other gamescope-ritz-wide settings exist yet -- see this file's "
				"comment before adding one that isn't asked for by the spec." );
		}
	}

	void PanelConfig_Draw()
	{
		EnsureInitialized();

		// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow(),
		// see Overlay/Chrome.h -- position/size unchanged from M7.
		if ( !chrome::BeginPanelWindow( "CONFIG / PROFILES", chrome::PanelId::Config,
			ImVec2( 520.0f, 380.0f ), ImVec2( 430.0f, 320.0f ) ) )
			return;

		if ( ImGui::BeginTabBar( "ConfigTabs" ) )
		{
			if ( ImGui::BeginTabItem( "Per-Game" ) )
			{
				DrawPerGameTab();
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "General" ) )
			{
				DrawGeneralTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		chrome::EndPanelWindow();
	}
}
