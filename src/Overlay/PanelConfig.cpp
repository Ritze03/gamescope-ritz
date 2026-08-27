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
#include "UI/Controls.h"   // controls::DeferToRelease() -- UI scale applies on release
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

		// Pushes the fields Widgets.cpp reads live (display scale, and the
		// window/dock opacities that outlived their surfaces)
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
			live.flDisplayScale = o.display_scale;
			live.flWindowAlphaFocused = o.opacity_windows_focused;
			live.flWindowAlphaUnfocused = o.opacity_windows_unfocused;
			live.flDockAlpha = o.opacity_dock;

			// NOTE (2026-08-24, D27): this function no longer runs on every
			// tick of a UI-scale drag. `overlay.display_scale`'s setter now
			// defers its whole apply -- this push included -- to the frame
			// after the drag ends (see that setter), so the paragraph below
			// describes a one-frame bridge between the committed value and
			// the atlas re-bake, not a per-tick preview. The division is
			// still exactly right for that bridge, and is still needed for
			// every non-drag write (arrow keys, the reset chip,
			// `overlay_e2_set`, a config reload).
			//
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

		// Everything a UI-scale change actually COSTS, in one place: the live
		// theme push (which reflows every rect in the overlay), the disk
		// write, and the font-atlas re-bake request.
		//
		// Split out of the setter so the drag can defer the WHOLE of it. An
		// earlier, smaller fix -- deferring only the re-bake -- would not have
		// helped: `palette::g_LiveTheme.flDisplayScale` is what every rect in
		// the kit multiplies by, so leaving that live still slides the track
		// out from under the pointer, which is the thing the user could not
		// aim at.
		//
		// #51's rule is untouched: this only REQUESTS the re-bake.
		// fonts::PumpRequestedRebuild() performs it at the top of the render
		// thread's next frame, so no atlas is ever swapped mid-frame, and this
		// call is safe from the console thread too.
		void ApplyDisplayScale()
		{
			QueueGeneralSave();
			gamescope::fonts::RequestRebuild( s_GeneralSettings.overlay.display_scale );
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
				.Help( "Gives this game its own settings, separate from everything else. Turn it on to "
				       "customise just this game; turning it off later keeps what you saved, so turning "
				       "it back on brings those settings back." )
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
					.Help( "Copies another game's settings into this game and switches this game to its "
					       "own settings. It's a one-time copy: later changes to that other game won't "
					       "carry over." )
					.Keywords( "copy clone import another game config" )
					.DisabledUnless( []{ return s_oAppId.has_value(); },
					                 "no game was identified for this session" )
					.Param( "source", "From",
						ui::AnyBind::Of<int>(
							[]{ return std::max( s_nSelectedCopyGame, 0 ); },
							[]( int n ) { s_nSelectedCopyGame = n; } ),
						s_vecGameOptions.data(), s_vecGameOptions.size() )
						.Help( "Which game's settings to copy from. Only games that have their own "
						       "settings show up here." )
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
					.Help( "Deletes this game's saved settings for good -- this can't be undone. Press "
					       "once to arm the button, then again to confirm." )
					.Keywords( "delete remove destroy per game config file" );
			}

			a.Group( "Diagnostics" );
			a.Facts( "config.routing", "Resolution order",
				[]{ return s_bOverrideActive ? std::string( "2 layers" ) : std::string( "global only" ); } )
				.Help( "Shows whether this game is using its own settings or the shared global "
				       "ones." )
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
					.Help( "Which saved profile to use. A profile is a saved bundle of settings you "
					       "can load again any time." )
					.Default( 0 )
					.Keywords( "profile preset saved snapshot pick" );

				a.Action( "profiles.apply", "Apply profile", "apply",
					[]{ ApplySelectedProfile(); } )
					.Help( "Loads the selected profile's settings right now. It's a one-time copy: "
					       "changing the profile later won't update settings you already applied." )
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
				.Help( "Name for your new profile. Letters, digits, spaces, hyphens and underscores "
				       "only." )
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
				.Help( "Saves your current settings as a new profile you can load again later." )
				.Keywords( "save profile new snapshot store" )
				.DisabledUnless( []{ return s_szNewProfileName[ 0 ] != '\0'; },
				                 "type a name first" );

			a.Group( "Diagnostics" );
			a.Facts( "profiles.facts", "Profiles",
				[]{ return std::to_string( s_ProfileNames.size() ) + " saved"; } )
				.Help( "Shows how many profiles you've saved and which one you used last." )
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
				.Help( "Changes the overlay's accent colour -- sliders, toggles and highlights all "
				       "follow it. Pick any colour; it's always kept easy to read." )
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

						// The STORED value always moves immediately, so the
						// row's own readout tracks the pointer and the user
						// can see what they are about to get. Only the APPLY
						// waits.
						//
						// APPLY ON RELEASE (the user, 2026-08-24: "The UI
						// scale should update, when the slider is released.
						// Otherwise, it is almost impossible, to adjust.").
						// This is the one setting in the product whose value
						// decides the geometry of the control editing it, so
						// a live apply moves the track out from under the
						// pointer mid-drag.
						//
						// NO PREVIEW DURING THE DRAG, deliberately. A preview
						// is exactly what the user was complaining about --
						// and the preview path was also #54's whole bug
						// surface: FontGlobalScale multiplies on top of the
						// *baked* atlas scale rather than 1.0, so a naive
						// per-tick preview drifts, and the BuiltScale()
						// division that corrects it only has to be right
						// because the preview exists. Not previewing removes
						// the class instead of correcting it again.
						//
						// The value is read back from the settings struct
						// inside ApplyDisplayScale() rather than captured
						// here: a deferred callable that captured `flScale`
						// would apply whichever tick happened to queue it,
						// and the drag's LAST tick is the one that should
						// land.
						if ( ui::IsPointerDragActive() )
						{
							ui::controls::DeferToRelease( []{ ApplyDisplayScale(); } );
							return;
						}
						ApplyDisplayScale();
					} ) )
				.Help( "Makes the whole overlay, including its text, bigger or smaller. Turn it up if "
				       "things are hard to read, or down to fit more on screen." )
				.Range( 0.5f, 2.0f )
				.Step( 0.05f )       // 31 positions: 0.50x, 0.55x, ... 2.00x
				.Default( config::OverlaySettings{}.display_scale )
				.Unit( "x" )
				.Keywords( "scale ui size dpi zoom display_scale font atlas" );
			// Notification scale used to hang off this row as a Param. It is
			// now a row of its own in the Notifications group below (the
			// user, 2026-08-24) -- it sizes the toasts, not the overlay, so
			// it belongs with the other toast settings rather than under a
			// slider it does not affect. The CONFIG KEY IS UNCHANGED
			// (overlay.notification_scale); this is a grouping change only.

			a.Group( "Backdrop" );

			a.Slider( "overlay.background_blur", "Backdrop blur",
				BindOverlayFloat( &config::OverlaySettings::background_blur ) )
				.Help( "Blurs the game behind the overlay while it's open. Higher makes the game "
				       "harder to see." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )       // 21 positions across a 0..1 amount
				.Default( config::OverlaySettings{}.background_blur )
				.Keywords( "blur backdrop frost background compositor" );

			a.Slider( "overlay.background_darkening", "Backdrop darkening",
				BindOverlayFloat( &config::OverlaySettings::background_darkening ) )
				.Help( "Dims the game behind the overlay while it's open. Turn it up to make the "
				       "overlay's text easier to read." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )       // 21 positions
				.Default( config::OverlaySettings{}.background_darkening )
				.Keywords( "darken dim backdrop veil contrast background" );

			a.Group( "Transparency" );

			a.Slider( "overlay.opacity_windows_focused", "Window (focused)",
				BindOverlayFloat( &config::OverlaySettings::opacity_windows_focused ) )
				.Help( "How see-through an overlay window is while you're using it." )
				.Range( 0.3f, 1.0f )
				.Step( 0.05f )       // 15 positions; every alpha default is on the grid
				.Default( config::OverlaySettings{}.opacity_windows_focused )
				.Keywords( "opacity transparency window focused alpha" );

			a.Slider( "overlay.opacity_windows_unfocused", "Window (unfocused)",
				BindOverlayFloat( &config::OverlaySettings::opacity_windows_unfocused ) )
				.Help( "How see-through an overlay window is when you're not actively using it." )
				.Range( 0.3f, 1.0f )
				.Step( 0.05f )       // 15 positions; every alpha default is on the grid
				.Default( config::OverlaySettings{}.opacity_windows_unfocused )
				.Keywords( "opacity transparency window unfocused alpha fade" );

			a.Slider( "overlay.opacity_dock", "Dock",
				BindOverlayFloat( &config::OverlaySettings::opacity_dock ) )
				.Help( "How see-through the dock bar is." )
				.Range( 0.3f, 1.0f )
				.Step( 0.05f )       // 15 positions; every alpha default is on the grid
				.Default( config::OverlaySettings{}.opacity_dock )
				.Keywords( "opacity transparency dock alpha" );

			a.Slider( "overlay.opacity_notifications", "Notifications",
				BindOverlayFloat( &config::OverlaySettings::opacity_notifications ) )
				.Help( "How see-through pop-up notifications are." )
				.Range( 0.3f, 1.0f )
				.Step( 0.05f )       // 15 positions; every alpha default is on the grid
				.Default( config::OverlaySettings{}.opacity_notifications )
				.Keywords( "opacity transparency notification toast alpha" );

			// The Notifications group. THIS FILE OPENS IT, and
			// Notifications::RegisterRows() below adds the rest of its rows
			// without opening a second one -- Area::Group() is a band marker,
			// not a lookup, so calling it twice with the same name would draw
			// two identically-titled headers.
			//
			// Split this way because the two halves genuinely have different
			// owners: notification_scale is one more global-json overlay
			// field, bound and persisted exactly like every other row in this
			// area (BindOverlayFloat + QueueGeneralSave, which is also what
			// pushes it live into Notifications::g_LiveTheme). Re-binding it
			// inside Notifications.cpp would have made a SECOND writer of
			// global.json's overlay object, against this panel's own cached
			// s_GeneralSettings -- the stale-cache clobber this file's
			// EnsureGeneralSettingsLoaded() comment already warns about.
			a.Group( "Notifications" );

			a.Slider( "overlay.notification_scale", "Notification scale",
				BindOverlayFloat( &config::OverlaySettings::notification_scale ) )
				.Help( "Makes pop-up notifications bigger or smaller, without changing the size of "
				       "the rest of the overlay." )
				.Range( 0.5f, 2.0f )
				.Step( 0.05f )       // 21 positions
				.Default( config::OverlaySettings{}.notification_scale )
				.Unit( "x" )
				.Keywords( "notification toast scale size" );

			// The rest of the group. Registered from Notifications.cpp
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
				.Help( "Shows how these appearance settings are currently saved." )
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
