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
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "Config/ConfigManager.h"
#include "Widgets.h"
#include "Chrome.h"
#include "Notifications.h"
#include "Palette.h"
#include "Fonts.h"
#include "../SettingsOverlay.h"

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		bool s_bInitialized = false;

		std::optional<std::string> s_oAppId;
		bool s_bOverrideActive = false;

		// Cached alongside s_bOverrideActive (refreshed by RefreshLists(),
		// same as s_ProfileNames/s_OtherGameIds below): whether
		// games/<AppId>.json exists on disk right now, regardless of
		// whether it's currently active. Issue #43 -- disabling the
		// override no longer deletes this file, so "off, but a saved
		// config is sitting there" is a real state the Per-Game tab needs
		// to show (and the delete button needs to gate on), not just
		// something inferred from s_bOverrideActive.
		bool s_bHasSavedPerGameConfig = false;

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

		// ---- window-chrome overhaul: General tab (scale/opacity/effects) --
		// Unlike the Per-Game tab's fields above, this DOES keep its own
		// cached config::Settings -- see DrawGeneralTab()'s own comment for
		// why these fields specifically can't reuse PanelDisplay/PanelShaders/
		// PanelAudio's EnsureConfigLoaded()+ResolveEffective() pattern
		// (overlay.* is global.json-only, never routed through the per-game
		// session file).
		bool s_bGeneralSettingsLoaded = false;
		config::Settings s_GeneralSettings;

		void RefreshLists()
		{
			s_bHasSavedPerGameConfig = s_oAppId.has_value() && config::HasSavedPerGameConfig( *s_oAppId );

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

		// overlay.* is process-level/global.json-only (ConfigSchema.h's own
		// comment on OverlaySettings) -- deliberately config::LoadGlobal(),
		// never config::ResolveEffective(config::SessionAppId()) the way the
		// Per-Game tab's own fields would: a per-game override file is
		// always written with its `overlay` object omitted entirely
		// (ConfigManager.cpp's SettingsToJson(), bIncludeOverlay=false for
		// every per-game/profile write), so resolving through the current
		// session's per-game file while an override is active would show
		// (and, on the next edit, persist) compiled *defaults* here instead
		// of the user's real General-tab preferences. Loaded once per
		// process, matching every other panel's "cache locally, push on
		// every edit" shape (PanelDisplay.cpp's EnsureConfigLoaded() is the
		// canonical example) -- and unlike those panels, this one never
		// needs a config::ConfigGeneration() reload check: profile-apply/
		// override-toggle (PanelConfig's own Per-Game tab, the only thing
		// that bumps that generation) never touch `overlay` at all
		// (ConfigManager.h's ApplyProfile() doc comment), so nothing outside
		// this tab itself can ever make s_GeneralSettings stale.
		void EnsureGeneralSettingsLoaded()
		{
			if ( s_bGeneralSettingsLoaded )
				return;
			s_bGeneralSettingsLoaded = true;
			s_GeneralSettings = config::LoadGlobal();
		}

		// Pushes the fields this worker's own Chrome.cpp/Widgets.cpp read
		// live (dock/display scale, window-focused/unfocused/dock opacity)
		// into gamescope::palette::g_LiveTheme, notification_scale/
		// opacity_notifications into gamescope::Notifications::g_LiveTheme
		// (Notifications.cpp's own consumer, wired the same way -- see that
		// file's Notifications.h comment), and background_blur/
		// background_darkening into gamescope::g_BackgroundLiveTheme
		// (SettingsOverlay.cpp's own consumer -- see SettingsOverlay.h's
		// comment) so every change is visible the very next frame -- the
		// task's own "must take effect live, not on restart" requirement.
		// Persisting the value here (below) still happens either way.
		void PushLiveTheme()
		{
			auto &live = gamescope::palette::g_LiveTheme;
			const auto &o = s_GeneralSettings.overlay;
			live.flDockScale = o.dock_scale;
			live.flDisplayScale = o.display_scale;
			live.flWindowAlphaFocused = o.opacity_windows_focused;
			live.flWindowAlphaUnfocused = o.opacity_windows_unfocused;
			live.flDockAlpha = o.opacity_dock;
			ImGui::GetIO().FontGlobalScale = o.display_scale;

			// Issue #37: hue-only accent picker. Regenerates every
			// kAccent*/Accent() token (Palette.h/.cpp) from the new hue --
			// must run after live.flAccentHue is set, same ordering as
			// Chrome.cpp's EnsureLiveThemeLoaded().
			live.flAccentHue = o.accent_hue;
			gamescope::palette::UpdateAccentFamily();

			auto &liveNotif = gamescope::Notifications::g_LiveTheme;
			liveNotif.flScale = o.notification_scale;
			liveNotif.flOpacity = o.opacity_notifications;

			auto &liveBackground = gamescope::g_BackgroundLiveTheme;
			liveBackground.flBlur = o.background_blur;
			liveBackground.flDarkening = o.background_darkening;
		}

		// overlay.* is process-level, never per-game/profile-routed (see
		// EnsureGeneralSettingsLoaded()'s comment) -- so this always writes
		// straight to global.json, unlike every other panel's QueueSave(),
		// which goes through config::EnqueueRoutedWrite() to respect
		// "Override Global Config".
		void QueueGeneralSave()
		{
			PushLiveTheme();
			config::EnqueueGlobalWrite( s_GeneralSettings );
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

		// "Override Global Config" ON (DECISIONS.md #19, amended -- issue
		// #43): if games/<AppId>.json already has saved values on disk (left
		// behind by a previous DisableOverride, which no longer deletes it --
		// see that function below), RESTORE them in place rather than
		// re-snapshotting from global. "I turned this off and then on again"
		// is expected to bring back what was there, not silently discard it
		// -- re-snapshotting here would reintroduce the exact data loss issue
		// #43 was about, one step later. Only when no saved file exists at
		// all does this fall back to the original behaviour: a FULL SNAPSHOT
		// of whatever is currently effective (global.json) -- not a sparse
		// delta, and later global.json edits won't reach this game from this
		// point on.
		void EnableOverride()
		{
			if ( !s_oAppId )
				return;

			if ( config::HasSavedPerGameConfig( *s_oAppId ) && config::RestorePerGameOverride( *s_oAppId ) )
			{
				s_sStatus = "Override enabled -- restored this game's previously saved config.";
			}
			else
			{
				config::Settings snapshot = config::ResolveEffective( s_oAppId );
				config::EnqueuePerGameSnapshot( *s_oAppId, snapshot );
				s_sStatus = "Override enabled -- current settings snapshotted for this game.";
			}

			config::SetSessionOverrideActive( true );
			config::BumpConfigGeneration();
			s_bOverrideActive = true;
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Ok );
			RefreshLists();
		}

		// "Override Global Config" OFF (issue #43): deactivates
		// games/<AppId>.json by flipping its own override_global field to
		// false IN PLACE -- it no longer deletes the file. This game falls
		// back to reading global.json again, per Feature 6's strictly-
		// two-level resolution (never a merge of both), but the saved
		// per-game values stay on disk so EnableOverride above can restore
		// them later. Deleting the file outright is now a separate,
		// explicit, confirmed action -- see DeleteSavedPerGameConfig().
		//
		// ponytail: called inline (not via an Enqueue* background-thread
		// path) -- ClearPerGameOverride() is a single atomic rewrite, and
		// this only runs once per checkbox click, not once per slider tick
		// the way EnqueueRoutedWrite() below does. The Enqueue* family
		// exists for the fsync()+rename() write paths that can be hit
		// continuously by a slider drag; a one-off rewrite from a button
		// click doesn't need the same treatment.
		void DisableOverride()
		{
			if ( !s_oAppId )
				return;
			config::ClearPerGameOverride( *s_oAppId );
			config::SetSessionOverrideActive( false );
			config::BumpConfigGeneration();
			s_bOverrideActive = false;
			s_sStatus = "Override disabled -- back to global. This game's saved config was kept, not deleted.";
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Info );
			RefreshLists();
		}

		// The only action in this panel that actually destroys a config file
		// (issue #43: "There can be a button for it, but never delete
		// configs automatically"). Only ever targets games/<AppId>.json for
		// the currently-resolved app id -- never global.json, never anything
		// under profiles/ -- and DeletePerGameOverride() itself refuses to
		// touch anything outside GamesDir() as a second layer of defense.
		// Called from the confirmation modal in DrawPerGameTab(), never
		// directly from the delete button.
		void DeleteSavedPerGameConfig()
		{
			if ( !s_oAppId )
				return;

			bool bOk = config::DeletePerGameOverride( *s_oAppId );
			if ( s_bOverrideActive )
			{
				config::SetSessionOverrideActive( false );
				config::BumpConfigGeneration();
				s_bOverrideActive = false;
			}
			s_sStatus = bOk
				? "Deleted this game's saved config. Back to global."
				: "Failed to delete this game's saved config.";
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Info : gamescope::Notifications::Kind::Error );
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
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Ok );
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
				// Spec §7 "Read-only readout strip", gap list item 11: a
				// system-verified value ("this is the app id we resolved"),
				// styled as never-editable-looking rather than a
				// full-brightness plain ImGui::Text() row, with the same 6x6
				// square status dot the title bar uses (not a circle).
				char szAppId[128];
				std::snprintf( szAppId, sizeof( szAppId ), "app id: %s", s_oAppId->c_str() );
				widgets::ReadoutStrip( szAppId, /*bLeadingDot=*/ true );
				ImGui::TextUnformatted( s_bOverrideActive
					? "Editing: this game's own config (games/<id>.json) -- frozen snapshot, independent of global."
					: "Editing: global config (global.json) -- shared with every other game that doesn't override it." );

				bool bOverride = s_bOverrideActive;
				if ( widgets::Toggle( "Override Global Config", &bOverride ) )
				{
					if ( bOverride )
						EnableOverride();
					else
						DisableOverride();
				}
				ImGui::TextDisabled(
					"Snapshots the current settings the first time you turn this on. If this game already "
					"has a saved config, turning it back on restores that instead of starting over." );

				// Issue #43's UI-honesty requirement: disabling the override
				// no longer deletes games/<AppId>.json (DisableOverride()'s
				// own comment above), so "off, but a saved config is still
				// sitting on disk" is a real, reachable state now -- without
				// this line, turning the toggle back on and getting the OLD
				// values back (EnableOverride()'s restore path, per the
				// user's own decision -- DECISIONS.md #19's amendment) would
				// look like a bug instead of the intended behaviour.
				if ( !s_bOverrideActive && s_bHasSavedPerGameConfig )
				{
					ImGui::TextColored( ImVec4( 0.55f, 0.75f, 0.95f, 1.0f ),
						"A saved config exists for this game (games/%s.json). "
						"Turning Override back on loads it -- it won't be re-created from global.",
						s_oAppId->c_str() );
				}

				// The only destructive action left in this panel (issue #43:
				// "It shouldnt do that. There can be a button for it, but
				// never delete configs automatically.") -- styled as clearly
				// destructive and gated behind a confirmation modal, since
				// this is now the sole path in the whole app that can ever
				// destroy a per-game config file. Only ever targets this
				// session's own games/<AppId>.json; DeletePerGameOverride()
				// itself refuses anything outside GamesDir() as a second
				// layer of defense (Config/ConfigManager.cpp).
				if ( s_bHasSavedPerGameConfig )
				{
					ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.18f, 0.16f, 1.0f ) );
					ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.70f, 0.22f, 0.18f, 1.0f ) );
					ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.80f, 0.26f, 0.20f, 1.0f ) );
					if ( ImGui::Button( "Delete Saved Config..." ) )
						ImGui::OpenPopup( "Delete Saved Config?" );
					ImGui::PopStyleColor( 3 );

					if ( ImGui::BeginPopupModal( "Delete Saved Config?", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
					{
						ImGui::TextWrapped(
							"This permanently deletes games/%s.json and everything saved in it. "
							"This cannot be undone.", s_oAppId->c_str() );
						ImGui::Separator();

						ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.18f, 0.16f, 1.0f ) );
						ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.70f, 0.22f, 0.18f, 1.0f ) );
						ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.80f, 0.26f, 0.20f, 1.0f ) );
						if ( ImGui::Button( "Delete Permanently", ImVec2( 160.0f, 0.0f ) ) )
						{
							DeleteSavedPerGameConfig();
							ImGui::CloseCurrentPopup();
						}
						ImGui::PopStyleColor( 3 );

						ImGui::SameLine();
						if ( ImGui::Button( "Cancel", ImVec2( 120.0f, 0.0f ) ) )
							ImGui::CloseCurrentPopup();

						ImGui::EndPopup();
					}
				}
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

		// One scale/opacity/effect slider, wired the same way every time:
		// widgets::SliderFloat edits the cached field directly, then
		// QueueGeneralSave() pushes it live (this worker's own fields) and
		// persists the whole cached struct to global.json.
		void DrawLiveFloatSlider( const char *pszLabel, float *pflValue, float flMin, float flMax, const char *pszFormat = "%.2f" )
		{
			if ( widgets::SliderFloat( pszLabel, pflValue, flMin, flMax, pszFormat ) )
				QueueGeneralSave();
		}

		// Issue #37: hue-only accent picker. A full-width hue gradient strip,
		// click/drag to set *pflHueDegrees (0..360) -- sampled from the exact
		// same OklchToImU32() the real accent family is built from (Palette.h/
		// .cpp), at the base token's own L/C (.74/.12), so what's shown here
		// is never an approximate rainbow that could visibly disagree with
		// the actual accent at some hue. A bespoke ImDrawList/InvisibleButton
		// control rather than a Widgets.h addition -- widgets::SliderFloat's
		// spec-exact geometry has no gradient-track mode, and this is its
		// only caller (out of this task's Widgets.* scope besides). Returns
		// true the frame the value changes, mirroring widgets::SliderFloat's
		// own return contract so callers can drive QueueGeneralSave() the
		// same way.
		bool DrawAccentHueGradient( float *pflHueDegrees )
		{
			ImDrawList *pDrawList = ImGui::GetWindowDrawList();
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const float flWidth = ImGui::GetContentRegionAvail().x;
			const float flHeight = 14.0f;

			constexpr int kStops = 24;
			for ( int i = 0; i < kStops; i++ )
			{
				const ImU32 colA = gamescope::palette::OklchToImU32( 0.74f, 0.12f, ( 360.0f * i ) / kStops );
				const ImU32 colB = gamescope::palette::OklchToImU32( 0.74f, 0.12f, ( 360.0f * ( i + 1 ) ) / kStops );
				const float x0 = pos.x + flWidth * ( (float)i / (float)kStops );
				const float x1 = pos.x + flWidth * ( (float)( i + 1 ) / (float)kStops );
				pDrawList->AddRectFilledMultiColor( ImVec2( x0, pos.y ), ImVec2( x1, pos.y + flHeight ), colA, colB, colB, colA );
			}

			float flNorm = *pflHueDegrees / 360.0f;
			flNorm = flNorm < 0.0f ? 0.0f : ( flNorm > 1.0f ? 1.0f : flNorm );
			const float flMarkerX = pos.x + flWidth * flNorm;
			pDrawList->AddLine( ImVec2( flMarkerX, pos.y - 2.0f ), ImVec2( flMarkerX, pos.y + flHeight + 2.0f ), gamescope::palette::White( 0.95f ), 2.0f );
			pDrawList->AddCircleFilled( ImVec2( flMarkerX, pos.y + flHeight * 0.5f ), 3.0f, gamescope::palette::Black( 0.8f ) );

			ImGui::InvisibleButton( "##accent_hue_gradient", ImVec2( flWidth, flHeight ) );

			bool bChanged = false;
			if ( ImGui::IsItemActive() )
			{
				float flMouseNorm = ( ImGui::GetIO().MousePos.x - pos.x ) / flWidth;
				flMouseNorm = flMouseNorm < 0.0f ? 0.0f : ( flMouseNorm > 1.0f ? 1.0f : flMouseNorm );
				const float flNewHue = flMouseNorm * 360.0f;
				if ( flNewHue != *pflHueDegrees )
				{
					*pflHueDegrees = flNewHue;
					bChanged = true;
				}
			}
			return bChanged;
		}

		// Issue #38: display_scale doubles as the effective font-atlas scale,
		// so on top of DrawLiveFloatSlider()'s usual live-push/persist this
		// one slider also needs to re-bake every ImGui context's atlas --
		// gamescope::fonts::RebuildAll() clears and re-adds every font across
		// all three contexts (SettingsOverlay, FpsDisplay, Notifications),
		// which is real work not to repeat on every intermediate drag value.
		// Debounced to the slider's release: widgets::SliderFloat() (like
		// stock ImGui::SliderFloat()) returns true on every tick while
		// dragging, so QueueGeneralSave() below still fires every tick same
		// as any other slider (matches every other field's existing "live as
		// you drag" behavior for the value itself), but the rebuild only
		// runs once, on the single frame ImGui::IsItemDeactivatedAfterEdit()
		// reports the item went inactive after having actually changed --
		// mouse release (or Enter after ctrl+click-to-type), not every tick.
		void DrawDisplayScaleSlider( float *pflValue )
		{
			if ( widgets::SliderFloat( "Display (overall UI)", pflValue, 0.8f, 1.4f ) )
				QueueGeneralSave();
			if ( ImGui::IsItemDeactivatedAfterEdit() )
				gamescope::fonts::RebuildAll( *pflValue );
		}

		// General settings for gamescope-ritz itself -- window-chrome
		// overhaul's own General-tab scale/opacity/effect controls
		// (dock/display/notification scale, window-focused/unfocused/dock/
		// notifications opacity, background blur/darkening), grouped into
		// spec §6 group blocks (this worker's own Widgets.h addition -- see
		// that header's BeginGroupBlock() comment; this is the group-block
		// API's first real caller). Every slider here takes effect on the
		// very next frame (QueueGeneralSave() -> PushLiveTheme()) -- see
		// that function's comment for which fields this worker's own
		// Chrome.cpp/Widgets.cpp consume directly vs. which are pushed on
		// for a sibling consumer (Notifications.*, SettingsOverlay.cpp) to
		// read live.
		//
		// Two named groups, matching the user's own naming: "Transparency"
		// (the four plain alpha sliders) and "Background Effects" (the two
		// native-compositor passes on the game layer itself -- blur and
		// darkening). The former "Background veil" slider (opacity_background)
		// is gone -- see ConfigSchema.h's comment on its removal.
		void DrawGeneralTab()
		{
			EnsureGeneralSettingsLoaded();
			auto &o = s_GeneralSettings.overlay;

			if ( widgets::BeginGroupBlock( "##accent" ) )
			{
				ImGui::TextUnformatted( "Accent Color" );
				ImGui::SameLine();
				{
					// Live swatch of the exact resulting accent -- reads
					// gamescope::palette::kAccent directly, so it reflects
					// whatever UpdateAccentFamily() last computed (including
					// on process start, before this tab is ever opened).
					const float flSwatch = ImGui::GetTextLineHeight();
					const ImVec2 p = ImGui::GetCursorScreenPos();
					ImDrawList *pDrawList = ImGui::GetWindowDrawList();
					pDrawList->AddRectFilled( p, ImVec2( p.x + flSwatch, p.y + flSwatch ), gamescope::palette::kAccent, 3.0f );
					pDrawList->AddRect( p, ImVec2( p.x + flSwatch, p.y + flSwatch ), gamescope::palette::White( 0.3f ), 3.0f );
					ImGui::Dummy( ImVec2( flSwatch, flSwatch ) );
				}

				if ( DrawAccentHueGradient( &o.accent_hue ) )
					QueueGeneralSave();
				ImGui::Spacing();
				DrawLiveFloatSlider( "Hue", &o.accent_hue, 0.0f, 360.0f, "%.0f deg" );
				ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
				ImGui::TextDisabled( "Saturation/lightness stay fixed -- only the hue rotates, so the accent"
					" family (sliders, toggles, dock, notifications) always keeps the design's contrast." );
				ImGui::PopFont();
			}
			widgets::EndGroupBlock();

			if ( widgets::BeginGroupBlock( "##scale" ) )
			{
				ImGui::TextUnformatted( "UI Scale" );
				DrawLiveFloatSlider( "Dock", &o.dock_scale, 0.85f, 1.5f );
				DrawDisplayScaleSlider( &o.display_scale );
				ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
				ImGui::TextDisabled( "Display scale re-bakes the font atlas at the new size on release, so"
					" text stays crisp -- widget/window geometry stays spec-exact, so very large values"
					" can overflow fixed-size controls." );
				ImGui::PopFont();
				DrawLiveFloatSlider( "Notifications", &o.notification_scale, 0.6f, 1.6f );
			}
			widgets::EndGroupBlock();

			if ( widgets::BeginGroupBlock( "##opacity" ) )
			{
				ImGui::TextUnformatted( "Transparency" );
				DrawLiveFloatSlider( "Window (focused)", &o.opacity_windows_focused, 0.3f, 1.0f );
				DrawLiveFloatSlider( "Window (unfocused)", &o.opacity_windows_unfocused, 0.3f, 1.0f );
				DrawLiveFloatSlider( "Dock", &o.opacity_dock, 0.3f, 1.0f );
				DrawLiveFloatSlider( "Notifications", &o.opacity_notifications, 0.3f, 1.0f );
			}
			widgets::EndGroupBlock();

			if ( widgets::BeginGroupBlock( "##effects" ) )
			{
				ImGui::TextUnformatted( "Background Effects" );
				DrawLiveFloatSlider( "Blur", &o.background_blur, 0.0f, 1.0f );
				DrawLiveFloatSlider( "Darkening", &o.background_darkening, 0.0f, 1.0f );
				ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
				ImGui::TextDisabled( "Native compositor pass -- drives FrameInfo_t::blurRadius / the game-layer dim multiply." );
				ImGui::PopFont();
			}
			widgets::EndGroupBlock();

			ImGui::Separator();
			ImGui::TextUnformatted( "Config directory:" );
			ImGui::TextWrapped( "%s", config::ConfigRoot().c_str() );
			ImGui::TextDisabled(
				"Lossless.dll path (LSFG-VK) is out of scope for this roadmap -- "
				"LSFG-VK integration was cut (see DECISIONS.md #1)." );
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
			// Toast notification system's own settings (global placement
			// picker, per-game/global mute) -- see Overlay/Notifications.h.
			// A new tab rather than a new dock panel: PanelId (Overlay/Chrome.h)
			// is a sibling worker's file this task must not touch, and a tab
			// here needs no changes there at all.
			if ( ImGui::BeginTabItem( "Notifications" ) )
			{
				gamescope::Notifications::DrawSettingsPanel();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		chrome::EndPanelWindow();
	}
}
