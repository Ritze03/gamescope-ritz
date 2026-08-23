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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Config/ConfigManager.h"
#include "UI/Registry.h"
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

		// Issue #43 recommendation #10: provenance readout, refreshed
		// alongside s_bHasSavedPerGameConfig/s_ProfileNames above by
		// RefreshLists() -- the name of whichever profile was last applied
		// to the file currently in effect (per-game if the override is
		// active, else global), read from that file's own
		// Settings::last_applied_profile (ConfigSchema.h). Empty means
		// "never applied", not "not loaded yet" -- see RefreshLists().
		std::string s_sLastAppliedProfile;

		// Issue #43 recommendation #1: one-frame-lagged tracking of whether
		// the tab drawn *last* frame was General/Notifications (always
		// global.json, see EnsureGeneralSettingsLoaded()'s comment below) --
		// see PanelConfig_Draw()'s own comment on why the badge can only
		// ever be a frame behind the tab bar's own selection, same
		// principle Chrome.cpp already accepts for s_bPanelWasFocused.
		bool s_bLastTabWasGlobalOnly = false;

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

			// Issue #43 recommendation #10: re-read whichever file is
			// currently authoritative (same "per-game if override active,
			// else global" rule ApplySelectedProfile()/SaveCurrentAsNewProfile()
			// already use below) for its provenance breadcrumb. A real read,
			// not free -- but this only runs on RefreshLists()'s own
			// trigger points (init + after an action), never once per frame.
			config::Settings current = ( s_bOverrideActive && s_oAppId )
				? config::ResolveEffective( s_oAppId )
				: config::LoadGlobal();
			s_sLastAppliedProfile = current.last_applied_profile;
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

			// Issue #54: FontGlobalScale folds on top of whatever this
			// context's atlas is *currently* baked at (Fonts.cpp's Load()'s
			// own comment on UpdateCurrentFontSize()), not on top of a fixed
			// 1.0x baseline -- so assigning o.display_scale here directly
			// only previews correctly the very first time a context is ever
			// scaled away from the compiled-in 1.0x default. On every later
			// drag the atlas is already baked at whatever the *previous*
			// release left it at (say 1.5x), so this same per-tick
			// assignment stacks the live drag value on top of that leftover
			// baked scale instead of replacing it: dragging from a 1.5x
			// atlas back down to 1.0x renders every implicit-size text call
			// (ImGui::Text()/Checkbox()/etc -- most of this UI; explicit-
			// size AddText() calls read DisplayScale() directly and are
			// unaffected) at a stuck 1.5x for the whole drag, then snaps to
			// the true 1.0x the instant DrawDisplayScaleSlider()'s
			// RebuildAll() re-bakes on release -- the reported "drastic
			// jump". Dividing out gamescope::fonts::BuiltScale() here keeps
			// the preview's implicit-size text tracking the live value the
			// same linear way Widgets.cpp/Chrome.cpp's DisplayScale()-driven
			// geometry already does (that path was already correct -- only
			// this text path was compounding), so RebuildAll()'s later
			// re-bake (and its own FontGlobalScale reset to 1.0, Fonts.cpp's
			// Load()) lands on the exact value the preview already showed:
			// no jump. BuiltScale() never returns 0, so this division is
			// always safe.
			//
			// GUARDED (P3b): every legacy caller of this function runs from
			// inside an ImGui frame, but `overlay.display_scale` is now a
			// registered row, and a registration's setter is also reachable
			// from `overlay_e2_set` -- which runs on the CONSOLE thread,
			// where there is no ImGui context at all. Unguarded, that is an
			// assert in ImGui::GetIO() and the compositor dies; it did.
			//
			// Skipping the write there is not a compromise: this line is
			// purely the live DRAG PREVIEW described above, and a console
			// write is not a drag. The value still reaches the UI, because
			// live.flDisplayScale below is what the kit reads, and
			// fonts::RebuildAll() re-bakes the atlas properly (it already
			// tolerates a null current context by design -- see Fonts.cpp).
			if ( ImGui::GetCurrentContext() != nullptr )
				ImGui::GetIO().FontGlobalScale = o.display_scale / gamescope::fonts::BuiltScale();

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

			// Issue #43 recommendation #10: set directly from `target`
			// (already in memory, and what config::ApplyProfile() just set
			// its own last_applied_profile to) rather than a RefreshLists()
			// disk re-read -- EnqueueRoutedWrite() above is a queued
			// background write, so a synchronous re-read here could race it
			// and briefly show the old value.
			s_sLastAppliedProfile = sName;
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

			// Issue #43 recommendation #10: provenance, not a live link --
			// DECISIONS.md #20 stays a one-time copy (RefreshLists()/
			// ApplySelectedProfile()'s own comments). "last applied", never
			// "current": editing settings afterward doesn't clear this, so
			// it names where the values *started*, not what's live now.
			if ( !s_sLastAppliedProfile.empty() )
				ImGui::TextDisabled( "last applied profile: %s", s_sLastAppliedProfile.c_str() );

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
			if ( widgets::SliderFloat( "Display (overall UI)", pflValue, 0.5f, 2.0f ) )
				QueueGeneralSave();
			if ( ImGui::IsItemDeactivatedAfterEdit() )
				gamescope::fonts::RebuildAll( *pflValue );
		}

		// Issue #43 recommendation #3: a small "reset" link, right-aligned
		// on a group's own header row. With live apply and no Cancel
		// anywhere in this overlay, reset *is* the undo -- and, until now,
		// there was none. ui-mockup-precise-spec.md §1 already reserves an
		// `accent-link-dim` token for exactly this ("footer action text --
		// reset, save as preset, live") -- specified, never built; this is
		// closing that spec gap, not inventing a new control.
		//
		// Deliberately a small local ImGui::SmallButton(), not a Widgets.h
		// addition -- a sibling worker is on Widgets.cpp right now (issue
		// #46, segmented-control label sizing), so this task stays out of
		// that file entirely. If a shared version is wanted later, this is
		// the one caller to fold into it.
		//
		// pszLabel follows ImGui's own "visible text##id" convention so
		// every call site can share the literal "reset" without an ID
		// collision (SmallButton()'s ID -- and therefore its click state --
		// is everything including and after "##", not just what's drawn).
		bool DrawResetLink( const char *pszLabel )
		{
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.42f, 0.80f, 0.85f, 0.85f ) ); // accent-link-dim-ish, ui-mockup-precise-spec.md §1
			ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
			ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 1.0f, 1.0f, 1.0f, 0.08f ) );
			ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 1.0f, 1.0f, 1.0f, 0.14f ) );
			const bool bClicked = ImGui::SmallButton( pszLabel );
			ImGui::PopStyleColor( 4 );
			ImGui::PopFont();
			return bClicked;
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
				if ( DrawResetLink( "reset##accent" ) )
				{
					o.accent_hue = config::OverlaySettings{}.accent_hue;
					QueueGeneralSave();
				}
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
				ImGui::SameLine();
				if ( DrawResetLink( "reset##scale" ) )
				{
					const config::OverlaySettings defaults{};
					o.dock_scale = defaults.dock_scale;
					o.display_scale = defaults.display_scale;
					o.notification_scale = defaults.notification_scale;
					QueueGeneralSave();
					// Same rebuild DrawDisplayScaleSlider() triggers on
					// release -- a direct assignment never fires that
					// slider's own IsItemDeactivatedAfterEdit() check.
					gamescope::fonts::RebuildAll( o.display_scale );
				}
				DrawLiveFloatSlider( "Dock", &o.dock_scale, 0.85f, 2.0f );
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
				ImGui::SameLine();
				if ( DrawResetLink( "reset##opacity" ) )
				{
					const config::OverlaySettings defaults{};
					o.opacity_windows_focused = defaults.opacity_windows_focused;
					o.opacity_windows_unfocused = defaults.opacity_windows_unfocused;
					o.opacity_dock = defaults.opacity_dock;
					o.opacity_notifications = defaults.opacity_notifications;
					QueueGeneralSave();
				}
				DrawLiveFloatSlider( "Window (focused)", &o.opacity_windows_focused, 0.3f, 1.0f );
				DrawLiveFloatSlider( "Window (unfocused)", &o.opacity_windows_unfocused, 0.3f, 1.0f );
				DrawLiveFloatSlider( "Dock", &o.opacity_dock, 0.3f, 1.0f );
				DrawLiveFloatSlider( "Notifications", &o.opacity_notifications, 0.3f, 1.0f );
			}
			widgets::EndGroupBlock();

			if ( widgets::BeginGroupBlock( "##effects" ) )
			{
				ImGui::TextUnformatted( "Background Effects" );
				ImGui::SameLine();
				if ( DrawResetLink( "reset##effects" ) )
				{
					const config::OverlaySettings defaults{};
					o.background_blur = defaults.background_blur;
					o.background_darkening = defaults.background_darkening;
					QueueGeneralSave();
				}
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

	// The panel's body, with no window around it -- see PanelConfig.h's
	// PanelConfig_DrawBody(). Verbatim from PanelConfig_Draw(), including
	// the s_bLastTabWasGlobalOnly write the title-bar badge reads a frame
	// later (the E2 sheet has no per-panel title bar, so there the write is
	// simply inert rather than wrong).
	static void DrawBodyContent()
	{
		bool bGlobalOnlyTabActiveThisFrame = false;
		if ( ImGui::BeginTabBar( "ConfigTabs" ) )
		{
			if ( ImGui::BeginTabItem( "Per-Game" ) )
			{
				DrawPerGameTab();
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "General" ) )
			{
				bGlobalOnlyTabActiveThisFrame = true;
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
				bGlobalOnlyTabActiveThisFrame = true;
				gamescope::Notifications::DrawSettingsPanel();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		s_bLastTabWasGlobalOnly = bGlobalOnlyTabActiveThisFrame;
	}

	void PanelConfig_Draw()
	{
		EnsureInitialized();

		// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow(),
		// see Overlay/Chrome.h -- position/size unchanged from M7.
		//
		// Issue #43 recommendation #1: this panel is the one place the
		// title-bar badge can't be computed from session-routing state
		// alone (Chrome.h's own comment on pszBadgeOverride) -- General/
		// Notifications always write global.json regardless of the
		// per-game override (EnsureGeneralSettingsLoaded()'s comment
		// below), a routing rule distinct from the Per-Game tab's. But
		// BeginPanelWindow() -- and therefore the title bar -- draws
		// *before* the tab bar below picks this frame's active tab, so the
		// override can only ever reflect *last* frame's tab. Same one-
		// frame lag Chrome.cpp already accepts for its own per-panel focus
		// state; harmless here since the badge only needs to catch up
		// within a frame of a tab switch, not synchronously.
		const char *pszBadgeOverride = s_bLastTabWasGlobalOnly ? "global only" : nullptr;
		if ( !chrome::BeginPanelWindow( "CONFIG / PROFILES", chrome::PanelId::Config,
			ImVec2( 520.0f, 380.0f ), ImVec2( 430.0f, 320.0f ), pszBadgeOverride ) )
			return;

		DrawBodyContent();

		chrome::EndPanelWindow();
	}

	void PanelConfig_DrawBody()
	{
		EnsureInitialized();
		DrawBodyContent();
	}

	// =====================================================================
	//  P3 part B -- the E2 registrations
	// =====================================================================
	// The Config/Profiles panel's three tabs become THREE AREAS, following
	// D13.1 exactly: the rail is the product's only navigation, and
	// index.html -- the declared tiebreaker -- lists setup.profiles,
	// setup.pergame and setup.appearance as separate rail items. A three-
	// group sheet would be the same tab bar redrawn as headings.
	//
	// Nothing about what these WRITE changed. Same config keys, same
	// EnableOverride/DisableOverride/ApplySelectedProfile/... functions,
	// same routing rules -- including the awkward one that makes the badge
	// necessary: Appearance always writes global.json, even for a game with
	// an override active, while Per-game's own toggle routes per session.
	namespace
	{
		// ---- the layer badge (issue #43) -------------------------------
		// "Where does what I change here get written?" -- the question a
		// settings UI must never leave ambiguous, and the one the old
		// title-bar badge answered with a one-frame lag off the active tab
		// (see PanelConfig_Draw's comment). As an Area::Badge it is derived
		// per area instead, so there is no lag and no shared flag.
		std::string RoutedBadge()
		{
			EnsureInitialized();
			if ( !s_oAppId )
				return "global";
			return s_bOverrideActive ? ( "app " + *s_oAppId ) : "global";
		}

		std::string PerGameFilePath()
		{
			return s_oAppId ? ( "games/" + *s_oAppId + ".json" ) : std::string( "(no app id)" );
		}

		// ---- profiles ---------------------------------------------------
		// ui::Option holds a const char *, and the profile list is refreshed
		// from disk, so the option text needs storage that outlives the
		// registration and never reallocates under a live pointer. Same
		// deque reasoning as PanelAudio.cpp's stream options.
		std::deque<std::string> s_dqProfileText;
		std::vector<ui::Option> s_vecProfileOptions;
		std::deque<std::string> s_dqGameText;
		std::vector<ui::Option> s_vecGameOptions;

		// Profiles and other-game configs are BOTH dynamic sets -- a profile
		// appears the moment one is saved, and another game's config appears
		// the moment that game first overrides. So Setup uses the same
		// rebuild mechanism Audio does (ui::Area::Rebuilds).
		uint64_t ConfigGenerationHash()
		{
			EnsureInitialized();

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

			Mix( s_oAppId ? *s_oAppId : std::string( "-" ) );
			Mix( s_bOverrideActive ? "override" : "global" );
			Mix( s_bHasSavedPerGameConfig ? "saved" : "nosaved" );
			for ( const std::string &s : s_ProfileNames )
				Mix( s );
			for ( const std::string &s : s_OtherGameIds )
				Mix( s );
			return ulHash;
		}

		void RebuildProfileOptions()
		{
			s_dqProfileText.clear();
			s_vecProfileOptions.clear();
			for ( size_t i = 0; i < s_ProfileNames.size(); ++i )
			{
				s_dqProfileText.emplace_back( s_ProfileNames[ i ] );
				s_vecProfileOptions.push_back( { (int)i, s_dqProfileText.back().c_str() } );
			}

			s_dqGameText.clear();
			s_vecGameOptions.clear();
			for ( size_t i = 0; i < s_OtherGameIds.size(); ++i )
			{
				s_dqGameText.emplace_back( "app " + s_OtherGameIds[ i ] );
				s_vecGameOptions.push_back( { (int)i, s_dqGameText.back().c_str() } );
			}
		}

		// ---- Per-game ---------------------------------------------------
		void BuildPerGameArea( ui::Area &a )
		{
			RebuildProfileOptions();

			a.Group( "Routing" );

			a.Switch( "config.override", "Override global config",
				ui::AnyBind::Of<bool>(
					[]{ return s_bOverrideActive; },
					[]( bool bOn )
					{
						if ( bOn )
							EnableOverride();
						else
							DisableOverride();
					} ) )
				.Help( "When on, every routed setting is written to this game's own file instead of "
				       "the global one. Turning it on the first time snapshots whatever is currently "
				       "effective. Turning it OFF keeps that file -- it is deactivated, never deleted "
				       "-- so turning it back on restores those values rather than starting over." )
				.Default( false )
				.Keywords( "override per game routing config file app id snapshot" )
				.DisabledUnless( []{ return s_oAppId.has_value(); },
					"no game was identified for this session -- gamescope-ritz was launched without "
					"GS_RITZ_APPID or a Steam app-id, so there is no per-game file to write. Every "
					"other panel is writing to global.json." );

			// Copying another game's config. The SOURCE is a parameter
			// rather than a second row: SPEC §5.3's whole argument is that
			// an action's operand is depth, and the sheet keeps the verb.
			if ( !s_OtherGameIds.empty() )
			{
				a.Action( "config.copy", "Copy another game's config", "copy",
					[]{ CopySelectedGameConfig(); } )
					.Help( "Replaces this game's config with a copy of another game's, and turns "
					       "Override Global Config on -- there is no other file a per-game copy could "
					       "land in. A one-time copy: later edits to that game do not follow." )
					.Keywords( "copy clone import another game config" )
					.DisabledUnless( []{ return s_oAppId.has_value(); },
					                 "no game was identified for this session" )
					.Param( "source", "From",
						ui::AnyBind::Of<int>(
							[]{ return std::max( s_nSelectedCopyGame, 0 ); },
							[]( int n ) { s_nSelectedCopyGame = n; } ),
						s_vecGameOptions.data(), s_vecGameOptions.size() )
						.Help( "Which game's saved config is copied. Only games that have their own "
						       "override are listed." )
						.Default( 0 );
			}

			// The ONE destructive action in the whole product. It is armed
			// by Confirm(), so a single press cannot delete anything -- the
			// user's rule, in the declaration rather than in a modal a call
			// site has to remember to open.
			if ( s_bHasSavedPerGameConfig )
			{
				a.Action( "config.delete", "Delete saved config", "delete...",
					[]{ DeleteSavedPerGameConfig(); } )
					.Confirm( "delete permanently?" )
					.Help( "Permanently deletes this game's saved config file and everything in it. "
					       "This cannot be undone, and it is the only action anywhere that destroys a "
					       "config -- nothing here ever deletes one on its own. Press once to arm, "
					       "again to confirm." )
					.Keywords( "delete remove destroy per game config file" );
			}

			a.Group( "Diagnostics" );
			a.Facts( "config.routing", "Resolution order",
				[]{ return s_bOverrideActive ? std::string( "2 layers" ) : std::string( "global only" ); } )
				.Help( "Where a value comes from when more than one file could supply it. Resolution "
				       "is strictly two-level and never a merge of both." )
				.Keywords( "routing resolution order layers provenance app id" )
				.Live( "app_id", []{
					return ui::Fact{ "resolved app id", s_oAppId ? *s_oAppId : "none -- no app id for this session" };
				} )
				.Live( "layer1", []{
					return ui::Fact{ "1 - per-game", s_bOverrideActive
						? ( PerGameFilePath() + "  (active)" )
						: ( s_bHasSavedPerGameConfig
							? ( PerGameFilePath() + "  (saved, not active)" )
							: std::string( "none" ) ) };
				} )
				.Live( "layer2", []{
					return ui::Fact{ "2 - global", s_bOverrideActive ? "global.json (shadowed)" : "global.json (active)" };
				} )
				.Live( "layer3", []{
					return ui::Fact{ "3 - default", "compiled in" };
				} )
				.Live( "root", []{
					return ui::Fact{ "config directory", config::ConfigRoot() };
				} )
				.Live( "status", []{
					return ui::Fact{ "last action", s_sStatus.empty() ? "none this session" : s_sStatus };
				} );
		}

		// ---- Profiles ---------------------------------------------------
		void BuildProfilesArea( ui::Area &a )
		{
			RebuildProfileOptions();

			a.Group( "Saved profiles" );

			if ( !s_ProfileNames.empty() )
			{
				a.Choice( "profiles.list", "Profile",
					ui::AnyBind::Of<int>(
						[]{ return std::max( s_nSelectedProfile, 0 ); },
						[]( int n ) { s_nSelectedProfile = n; } ),
					s_vecProfileOptions.data(), s_vecProfileOptions.size() )
					.Help( "Which saved profile the apply action targets. A profile is a named "
					       "snapshot of every setting, taken when it was saved." )
					.Default( 0 )
					.Keywords( "profile preset saved snapshot pick" );

				a.Action( "profiles.apply", "Apply profile", "apply",
					[]{ ApplySelectedProfile(); } )
					.Help( "Copies the selected profile's values into whichever file is currently "
					       "authoritative. A ONE-TIME copy: editing the profile afterwards does not "
					       "retroactively change anything it was applied to." )
					.Keywords( "apply profile preset load restore" );
			}

			a.Group( "New profile" );

			a.Text( "profiles.name", "Name",
				ui::AnyBind::Of<std::string>(
					[]{ return std::string( s_szNewProfileName ); },
					[]( std::string s )
					{
						std::snprintf( s_szNewProfileName, sizeof( s_szNewProfileName ), "%s", s.c_str() );
					} ) )
				.Help( "Name for a new profile. Letters, digits, space, hyphen and underscore only -- "
				       "the name becomes a filename, so it is sanitised before it is ever used as a "
				       "path." )
				.Keywords( "profile name new save" )
				.Validate( []( const std::string &s ) -> std::string
				{
					if ( s.empty() )
						return "";
					if ( !config::SanitizeProfileName( s ) )
						return "letters, digits, space, hyphen and underscore only";
					if ( std::find( s_ProfileNames.begin(), s_ProfileNames.end(), s ) != s_ProfileNames.end() )
						return "a profile with that name already exists";
					return "";
				} );

			a.Action( "profiles.save", "Save current settings", "save as profile",
				[]{ SaveCurrentAsNewProfile(); } )
				.Help( "Writes whatever config is currently in effect into a new named profile." )
				.Keywords( "save profile new snapshot store" )
				.DisabledUnless( []{ return s_szNewProfileName[ 0 ] != '\0'; },
				                 "type a name first" );

			a.Group( "Diagnostics" );
			a.Facts( "profiles.facts", "Profiles",
				[]{ return std::to_string( s_ProfileNames.size() ) + " saved"; } )
				.Help( "What is saved and what was last applied. Read-only." )
				.Keywords( "profile provenance last applied count diagnostics" )
				.Live( "count", []{
					return ui::Fact{ "saved profiles", s_ProfileNames.empty()
						? "none yet" : std::to_string( s_ProfileNames.size() ) };
				} )
				// Issue #43 recommendation #10: provenance, not a live link.
				// "last applied", never "current" -- editing settings
				// afterwards does not clear it, so it names where the values
				// STARTED, not what is live now.
				.Live( "last_applied", []{
					return ui::Fact{ "last applied profile", s_sLastAppliedProfile.empty()
						? "none has ever been applied to this config"
						: s_sLastAppliedProfile };
				} )
				.Live( "target", []{
					return ui::Fact{ "apply writes to", s_bOverrideActive && s_oAppId
						? PerGameFilePath() : std::string( "global.json" ) };
				} );
		}

		// ---- Appearance -------------------------------------------------
		// Every row here writes global.json unconditionally, which is why
		// this area's badge reads "global only" regardless of the per-game
		// override. That routing rule predates E2 (see
		// EnsureGeneralSettingsLoaded) and is unchanged.
		ui::AnyBind BindOverlayFloat( float config::OverlaySettings::*pField )
		{
			return ui::AnyBind::Of<float>(
				[ pField ]() -> float
				{
					EnsureGeneralSettingsLoaded();
					return s_GeneralSettings.overlay.*pField;
				},
				[ pField ]( float flValue )
				{
					EnsureGeneralSettingsLoaded();
					s_GeneralSettings.overlay.*pField = flValue;
					QueueGeneralSave();
				} );
		}

		void BuildAppearanceArea( ui::Area &a )
		{
			a.Group( "Theme" );

			// SPEC §4.4's "Accent hue" composite: a 2-line band whose body is
			// the hue rail plus its eight preset swatches, and whose value
			// column reads a plain `218°`.
			//
			// This was issue #37's gradient control, which P3b had to
			// downgrade to a plain Slider because Kind::Composite rendered
			// nothing at the time -- the hue was still settable, but the
			// strip that shows WHICH hue, sampled from the real
			// OklchToImU32(), was gone. It is restored here, on the band, not
			// as a second bespoke widget: Controls.cpp's Rail() samples the
			// same accent math the legacy strip did, so the two cannot
			// disagree about what a hue looks like.
			a.Composite( "overlay.accent_hue", "Accent colour", ui::CompositeKind::Hue,
				ui::AnyBind::Of<float>(
					[]{ EnsureGeneralSettingsLoaded(); return s_GeneralSettings.overlay.accent_hue; },
					[]( float flHue )
					{
						EnsureGeneralSettingsLoaded();
						s_GeneralSettings.overlay.accent_hue = flHue;
						// QueueGeneralSave() pushes the live theme before it
						// enqueues the write, so every accent token is
						// recomputed on the same frame the rail moves --
						// which is what makes the band's own swatches, and
						// the rest of the overlay, follow the drag.
						QueueGeneralSave();
					} ) )
				.Help( "One hue drives the whole accent family -- sliders, toggles, the rail's active "
				       "edge, notifications. Saturation and lightness are fixed per role, so no hue "
				       "can wash out or blow out the design's contrast." )
				.Range( 0.0f, 360.0f )
				.Default( config::OverlaySettings{}.accent_hue )
				.Unit( "deg" )
				.Keywords( "accent colour hue theme tint" );

			a.Slider( "overlay.display_scale", "UI scale",
				ui::AnyBind::Of<float>(
					[]{ EnsureGeneralSettingsLoaded(); return s_GeneralSettings.overlay.display_scale; },
					[]( float flScale )
					{
						EnsureGeneralSettingsLoaded();
						s_GeneralSettings.overlay.display_scale = flScale;
						QueueGeneralSave();
						// The font atlas is re-baked at the new size. The
						// legacy slider did this on release via
						// IsItemDeactivatedAfterEdit; a registration has no
						// such hook, so it happens on the write.
						gamescope::fonts::RebuildAll( flScale );
					} ) )
				.Help( "Multiplies every base unit in the overlay and re-bakes the font atlas, so "
				       "text stays crisp. Widget geometry stays spec-exact, so very large values can "
				       "overflow fixed-size controls. The responsive ladder decides what collapses "
				       "as this rises." )
				.Range( 0.5f, 2.0f )
				.Default( config::OverlaySettings{}.display_scale )
				.Unit( "x" )
				.Keywords( "scale ui size dpi zoom display_scale font atlas" )
				.Param( "dock", "Dock scale", BindOverlayFloat( &config::OverlaySettings::dock_scale ) )
					.Help( "Size of the legacy dock, independently of the overall UI scale." )
					.Range( 0.85f, 2.0f )
					.Default( config::OverlaySettings{}.dock_scale )
				.Param( "notifications", "Notification scale",
					BindOverlayFloat( &config::OverlaySettings::notification_scale ) )
					.Help( "Size of toast notifications, independently of the overall UI scale." )
					.Range( 0.6f, 1.6f )
					.Default( config::OverlaySettings{}.notification_scale );

			a.Group( "Backdrop" );

			a.Slider( "overlay.background_blur", "Backdrop blur",
				BindOverlayFloat( &config::OverlaySettings::background_blur ) )
				.Help( "How much the game behind the overlay is blurred. A native compositor pass -- "
				       "it drives FrameInfo_t's blur radius, not a shader effect." )
				.Range( 0.0f, 1.0f )
				.Default( config::OverlaySettings{}.background_blur )
				.Keywords( "blur backdrop frost background compositor" );

			a.Slider( "overlay.background_darkening", "Backdrop darkening",
				BindOverlayFloat( &config::OverlaySettings::background_darkening ) )
				.Help( "How far the game behind the overlay is dimmed. The overlay's contrast is "
				       "measured against this, so lowering it lowers legibility." )
				.Range( 0.0f, 1.0f )
				.Default( config::OverlaySettings{}.background_darkening )
				.Keywords( "darken dim backdrop veil contrast background" );

			a.Group( "Transparency" );

			a.Slider( "overlay.opacity_windows_focused", "Window (focused)",
				BindOverlayFloat( &config::OverlaySettings::opacity_windows_focused ) )
				.Help( "Opacity of an overlay window while it holds input focus." )
				.Range( 0.3f, 1.0f )
				.Default( config::OverlaySettings{}.opacity_windows_focused )
				.Keywords( "opacity transparency window focused alpha" );

			a.Slider( "overlay.opacity_windows_unfocused", "Window (unfocused)",
				BindOverlayFloat( &config::OverlaySettings::opacity_windows_unfocused ) )
				.Help( "Opacity of an overlay window while another surface holds input focus." )
				.Range( 0.3f, 1.0f )
				.Default( config::OverlaySettings{}.opacity_windows_unfocused )
				.Keywords( "opacity transparency window unfocused alpha fade" );

			a.Slider( "overlay.opacity_dock", "Dock",
				BindOverlayFloat( &config::OverlaySettings::opacity_dock ) )
				.Help( "Opacity of the legacy dock strip." )
				.Range( 0.3f, 1.0f )
				.Default( config::OverlaySettings{}.opacity_dock )
				.Keywords( "opacity transparency dock alpha" );

			a.Slider( "overlay.opacity_notifications", "Notifications",
				BindOverlayFloat( &config::OverlaySettings::opacity_notifications ) )
				.Help( "Opacity of toast notifications." )
				.Range( 0.3f, 1.0f )
				.Default( config::OverlaySettings{}.opacity_notifications )
				.Keywords( "opacity transparency notification toast alpha" );

			// The Config panel's third tab. Registered from Notifications.cpp
			// because everything it binds to is file-static there -- see
			// Notifications.h's RegisterRows() comment.
			gamescope::Notifications::RegisterRows( a );

			a.Group( "Diagnostics" );
			a.Facts( "overlay.appearance_facts", "Appearance",
				[]{
					EnsureGeneralSettingsLoaded();
					char sz[ 64 ];
					std::snprintf( sz, sizeof( sz ), "hue %.0f deg  ·  scale %.2fx",
						s_GeneralSettings.overlay.accent_hue, s_GeneralSettings.overlay.display_scale );
					return std::string( sz );
				} )
				.Help( "Where these settings are stored, and what the font atlas is currently baked "
				       "at. Read-only." )
				.Keywords( "appearance diagnostics global routing atlas scale" )
				.Live( "routing", []{
					return ui::Fact{ "written to",
						"global.json always -- overlay appearance is process-level, so a per-game "
						"override does not apply to it" };
				} )
				.Live( "atlas", []{
					char sz[ 48 ];
					std::snprintf( sz, sizeof( sz ), "%.2fx", gamescope::fonts::BuiltScale() );
					return ui::Fact{ "font atlas baked at", sz };
				} )
				.Live( "root", []{
					return ui::Fact{ "config directory", config::ConfigRoot() };
				} );
		}
	}

	void PanelConfig_RegisterAreas( ui::Registry &reg )
	{
		// ---- Profiles ------------------------------------------------
		ui::Area &profiles = reg.Add( "setup.profiles", "Profiles", ui::Section::Setup );
		profiles.Keywords( "profile preset snapshot save apply named config" );
		profiles.Summary( []{
			EnsureInitialized();
			return std::to_string( s_ProfileNames.size() ) + " saved";
		} );
		profiles.Badge( RoutedBadge );
		profiles.Rebuilds( ConfigGenerationHash, BuildProfilesArea );

		// ---- Per-game ------------------------------------------------
		ui::Area &pergame = reg.Add( "setup.pergame", "Per-game", ui::Section::Setup );
		pergame.Keywords( "per game override routing config file app id copy delete" );
		pergame.Summary( []{
			EnsureInitialized();
			if ( !s_oAppId )
				return std::string( "no game identified -- everything writes to global.json" );
			return s_bOverrideActive
				? ( "override on  ·  " + PerGameFilePath() )
				: std::string( "global only" );
		} );
		pergame.Badge( RoutedBadge );
		pergame.Rebuilds( ConfigGenerationHash, BuildPerGameArea );

		// ---- Appearance ----------------------------------------------
		// Not dynamic -- its row set is fixed. Built once, here.
		ui::Area &appearance = reg.Add( "setup.appearance", "Appearance", ui::Section::Setup );
		appearance.Keywords( "appearance theme accent hue scale opacity backdrop blur notification" );
		appearance.Summary( []{
			EnsureGeneralSettingsLoaded();
			char sz[ 96 ];
			std::snprintf( sz, sizeof( sz ), "hue %.0f deg  ·  scale %.2fx",
				s_GeneralSettings.overlay.accent_hue, s_GeneralSettings.overlay.display_scale );
			return std::string( sz );
		} );
		// Always global, even for a game with an override active -- the one
		// routing rule the session state cannot express, and the reason the
		// old title bar needed a per-tab override at all.
		appearance.Badge( []{ return std::string( "global only" ); } );
		BuildAppearanceArea( appearance );
	}
}
