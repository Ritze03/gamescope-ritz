// The Setup section's Profiles / Per-game / Appearance areas -- see
// PanelConfig.h, superdoc/features/profiles-and-per-game.md, and
// superdoc/planning/SPEC.md's Feature 6 ("Config system").
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
//
// WRITES FROM THIS PANEL ARE SYNCHRONOUS (requests-2026-09-05 item 3).
// Every action here is one button press, not a slider tick, and every one
// of them is immediately followed by RefreshLists() re-reading the
// directory -- so the write has to have landed by then. The Enqueue*
// background-writer family (ConfigManager.h) exists for the per-tick
// writes the other panels make; used here it produced the bug the user
// reported ("I need to restart the whole game to actually see a freshly
// created profile"): the write was still queued when the list was
// re-read, the cached list never moved, the Rebuilds() hash never moved,
// and the new profile did not appear until the next process. The only
// exception is the routed write of a Use/Restore (EnqueueRoutedWrite),
// which nothing here re-reads afterwards -- the panel updates its own
// state from the value it just wrote.
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
		// Per-game's "Start from profile" picks from the same list as
		// Profiles' "Use this profile" but keeps its own selection, so
		// browsing one area never silently changes what the other's button
		// would do.
		int s_nStartFromProfile = -1;
		char s_szNewProfileName[ 64 ] = {};
		// Phase B: the "Rename profile" row's target name. Its own buffer,
		// not s_szNewProfileName's -- a registry Param cannot be a Text
		// control (Registry.cpp's AddParam(): options mean Choice, a range
		// means Slider, neither means Switch), so the name is a sibling Text
		// row, and sharing the Save group's field would make "Save as new"
		// and "Rename" race for the same text.
		char s_szRenameTo[ 64 ] = {};

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

		// The one-step backup behind "Use this profile" / "Start from
		// profile" -- see panelconfig::SettingsBackup (PanelConfig.h) for
		// what it holds and why it lives in memory only. Set by UseProfile(),
		// consumed by RestorePreviousSettings(). Its presence is part of
		// ConfigGenerationHash(), which is how the "Restore previous
		// settings" row appears and disappears.
		std::optional<panelconfig::SettingsBackup> s_oBackup;

		// ---- window-chrome overhaul: General tab (scale/opacity/effects) --
		// Unlike the Per-Game tab's fields above, this DOES keep its own
		// cached config::Settings -- see DrawGeneralTab()'s own comment for
		// why these fields specifically can't reuse PanelDisplay/PanelShaders/
		// PanelAudio's EnsureConfigLoaded()+ResolveEffective() pattern
		// (overlay.* is global.json-only, never routed through the per-game
		// session file).
		bool s_bGeneralSettingsLoaded = false;
		config::Settings s_GeneralSettings;

		// Whichever file is authoritative for this session right now --
		// games/<AppId>.json while "Use separate settings for this game" is
		// on, global.json otherwise -- as the user is RUNNING it:
		// config::CurrentRoutedSettings() answers from ConfigManager's
		// in-memory mirror of the last routed write, falling back to disk
		// only when nothing has been written this process. Since Phase B's
		// write coalescing, a disk read here could be up to half a second
		// behind a slider another area just moved; the mirror never is.
		// Every action that copies something in (a profile, another game's
		// config) or out (save as new profile, save changes) starts from this.
		config::Settings CurrentTarget()
		{
			return config::CurrentRoutedSettings();
		}

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

			// Both sides clamped, not just the too-large one: the pickers
			// draw std::max( n, 0 ), so a -1 left over from before the list
			// was first read made the picker SHOW the first profile while
			// every action on "the selected profile" silently did nothing.
			// See panelconfig::ClampPickerSelection().
			s_nSelectedProfile  = panelconfig::ClampPickerSelection( s_nSelectedProfile,  s_ProfileNames.size() );
			s_nStartFromProfile = panelconfig::ClampPickerSelection( s_nStartFromProfile, s_ProfileNames.size() );
			s_nSelectedCopyGame = panelconfig::ClampPickerSelection( s_nSelectedCopyGame, s_OtherGameIds.size() );

			// Issue #43 recommendation #10: re-read whichever file is
			// currently authoritative (same "per-game if override active,
			// else global" rule UseProfile()/SaveCurrentAsNewProfile()
			// already use below) for its provenance breadcrumb. A real read,
			// not free -- but this only runs on RefreshLists()'s own
			// trigger points (init + after an action), never once per frame.
			s_sLastAppliedProfile = CurrentTarget().last_applied_profile;
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
		//
		// And every action here WRITES SYNCHRONOUSLY -- see the file-level
		// comment. The precedent is DisableOverride() below, which always
		// did: a one-off atomic rewrite from a button press does not need
		// the background writer that per-tick slider edits do.

		// "Use separate settings for this game" ON (DECISIONS.md #19,
		// amended -- issue #43): if games/<AppId>.json already has saved
		// values on disk (left behind by a previous DisableOverride, which
		// no longer deletes it -- see that function below), RESTORE them in
		// place rather than re-snapshotting from global. "I turned this off
		// and then on again" is expected to bring back what was there, not
		// silently discard it -- re-snapshotting here would reintroduce the
		// exact data loss issue #43 was about, one step later. Only when no
		// saved file exists at all does this fall back to the original
		// behaviour: a FULL SNAPSHOT of whatever is currently effective
		// (global.json) -- not a sparse delta, and later global.json edits
		// won't reach this game from this point on.
		//
		// Returns the one-line status for the caller to toast, so "Start
		// from profile" (which enables and then copies a profile in, in one
		// press) can show a single message instead of two.
		std::string EnableOverrideNoToast()
		{
			if ( !s_oAppId )
				return {};

			std::string sStatus;
			if ( config::HasSavedPerGameConfig( *s_oAppId ) && config::RestorePerGameOverride( *s_oAppId ) )
			{
				sStatus = "This game now uses its own settings -- the ones you saved for it before.";
			}
			else
			{
				// Synchronous on purpose: RefreshLists() right after this
				// reads HasSavedPerGameConfig() from disk, and the Delete
				// row's existence hangs off that answer. Queued, the file was
				// not there yet, the hash did not move, and the row was
				// missing until something else rebuilt the area.
				config::SnapshotPerGameOverride( *s_oAppId, CurrentTarget() );
				sStatus = "This game now uses its own settings -- a copy of your current ones.";
			}

			config::SetSessionOverrideActive( true );
			config::BumpConfigGeneration();
			s_bOverrideActive = true;
			s_sStatus = sStatus;
			RefreshLists();
			return sStatus;
		}

		void EnableOverride()
		{
			std::string sStatus = EnableOverrideNoToast();
			if ( !sStatus.empty() )
				gamescope::Notifications::Show( sStatus, gamescope::Notifications::Kind::Ok );
		}

		// "Use separate settings for this game" OFF (issue #43): deactivates
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
		// click doesn't need the same treatment. (Since item 3 this is the
		// rule for the whole panel, not the exception -- see the file-level
		// comment.)
		void DisableOverride()
		{
			if ( !s_oAppId )
				return;
			config::ClearPerGameOverride( *s_oAppId );
			config::SetSessionOverrideActive( false );
			config::BumpConfigGeneration();
			s_bOverrideActive = false;
			s_sStatus = "This game uses the shared settings again. What you saved for it was kept, not deleted.";
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Info );
			RefreshLists();
		}

		// The only action in this panel that actually destroys a config file
		// (issue #43: "There can be a button for it, but never delete
		// configs automatically"). Only ever targets games/<AppId>.json for
		// the currently-resolved app id -- never global.json, never anything
		// under profiles/ -- and DeletePerGameOverride() itself refuses to
		// touch anything outside GamesDir() as a second layer of defense.
		// Armed by the row's Confirm(), never a single press.
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
				? "Deleted this game's saved settings. Back to the shared settings."
				: "Failed to delete this game's saved settings.";
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Info : gamescope::Notifications::Kind::Error );
			RefreshLists();
		}

		// "Use this profile" (Profiles) and the second half of "Start from
		// profile" (Per-game): copies profile sName into whichever file is
		// authoritative right now -- a ONE-TIME COPY (DECISIONS.md #20): the
		// target has no memory of which profile it came from, and later
		// edits to the profile do not retroactively affect it.
		//
		// Takes the one-step backup FIRST (DECISIONS.md #20, extended): the
		// old Apply overwrote the user's live settings wholesale with no way
		// back, which is the "scared to touch it" the user reported. The
		// backup is the safety net, so Use itself needs no confirm step.
		//
		// Sets s_sStatus; does not toast -- the caller does, so the two
		// entry points can word their own messages. Returns false, changing
		// nothing, if the profile cannot be read.
		bool UseProfile( const std::string &sName )
		{
			// CurrentRoutedSettings(), not a disk read: another panel's
			// slider write may still be sitting in the (coalesced) queue,
			// and the backup has to hold what the user is actually running.
			config::Settings target = config::CurrentRoutedSettings();
			panelconfig::SettingsBackup backup{ target, sName, s_bOverrideActive, config::ActiveProfile() };

			if ( !config::ApplyProfile( target, sName ) )
			{
				s_sStatus = "Couldn't read profile '" + sName + "'. Nothing was changed.";
				return false;
			}

			s_oBackup = std::move( backup );

			// Phase B: this profile is now the active one -- set BEFORE the
			// routed write below, so that write's auto-save fan-out (if on)
			// targets the profile just used, not the one before it. (It
			// writes the profile's own values back into it: a no-op.)
			config::SetActiveProfile( sName );

			// The one queued write left in this panel -- and the one that
			// does not race anything: nothing below re-reads the file. The
			// panel's own view (s_sLastAppliedProfile) is set from `target`,
			// which is exactly what ApplyProfile() just wrote into it.
			config::EnqueueRoutedWrite( target );
			config::BumpConfigGeneration();
			s_sLastAppliedProfile = sName;
			s_sStatus = "Now using profile '" + sName + "'. Your previous settings are kept as a backup.";
			return true;
		}

		void UseSelectedProfile()
		{
			if ( s_nSelectedProfile < 0 || s_nSelectedProfile >= (int)s_ProfileNames.size() )
				return;
			const bool bOk = UseProfile( s_ProfileNames[ s_nSelectedProfile ] );
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Ok : gamescope::Notifications::Kind::Error );
		}

		// "Restore previous settings": writes the backup UseProfile() took
		// back to the file it came from, then forgets it (one step back,
		// never two). Refuses -- and the row is disabled with a reason --
		// when the routing has changed since the backup was taken; see
		// panelconfig::BackupMatchesRouting().
		void RestorePreviousSettings()
		{
			if ( !s_oBackup || !panelconfig::BackupMatchesRouting( *s_oBackup, s_bOverrideActive ) )
				return;

			// Active profile back first -- see SettingsBackup's own comment
			// on sPreviousActiveProfile for why the order matters.
			config::SetActiveProfile( s_oBackup->sPreviousActiveProfile );
			config::EnqueueRoutedWrite( s_oBackup->settings );
			config::BumpConfigGeneration();
			s_sLastAppliedProfile = s_oBackup->settings.last_applied_profile;
			s_sStatus = "Restored the settings you had before using '" + s_oBackup->sReplacedBy + "'.";
			s_oBackup.reset();
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Ok );
		}

		// Per-game's "Start from profile": the explicit form of what used to
		// take two steps in two areas (turn the override on here, then go
		// Apply over in Profiles). Turns separate settings on if they are
		// off, then copies the chosen profile into this game's own file.
		// Same one-step backup as Use -- of this game's own settings as
		// they were just before the copy (for a game whose separate
		// settings were just switched on, that is the copy of global it
		// started from, or the saved values a previous "off" left behind).
		void StartFromProfile()
		{
			if ( !s_oAppId || s_nStartFromProfile < 0 || s_nStartFromProfile >= (int)s_ProfileNames.size() )
				return;
			const std::string sName = s_ProfileNames[ s_nStartFromProfile ];

			if ( !s_bOverrideActive )
				EnableOverrideNoToast();

			const bool bOk = UseProfile( sName );
			if ( bOk )
				s_sStatus = "This game now uses its own settings, starting from profile '" + sName +
				            "'. Its previous settings are kept as a backup.";
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Ok : gamescope::Notifications::Kind::Error );
		}

		// "Save as new profile": saves whichever config is currently in
		// effect (per-game if separate settings are on, else global) under
		// a brand new name -- SanitizeProfileName() (reused from M0) is what
		// keeps user text input from ever escaping profiles/ as a path.
		//
		// SYNCHRONOUS -- this is the function the user's report was about.
		// config::SaveProfile() has returned with the file on disk before
		// RefreshLists() lists the directory, so the new name is in
		// s_ProfileNames, the Rebuilds() hash moves, and the picker shows it
		// on the very next frame. The old EnqueueProfileWrite() lost that
		// race every time.
		void SaveCurrentAsNewProfile()
		{
			std::optional<std::string> oSanitized = config::SanitizeProfileName( s_szNewProfileName );
			if ( !oSanitized )
			{
				s_sStatus = "Profile name can't be empty (letters/digits/space/-/_ only).";
				gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Warning );
				return;
			}

			if ( !config::SaveProfile( *oSanitized, config::CurrentRoutedSettings() ) )
			{
				s_sStatus = "Couldn't save profile '" + *oSanitized + "'.";
				gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Error );
				return;
			}

			// Phase B: a freshly saved profile IS the current settings, so it
			// becomes the active one -- "changes since applied: none" from
			// this moment, and auto-save (if on) keeps it that way.
			config::SetActiveProfile( *oSanitized );

			s_sStatus = "Saved profile '" + *oSanitized + "'. It's in the list above.";
			s_szNewProfileName[ 0 ] = '\0';
			RefreshLists();

			// Select what was just saved, so "Use this profile" is one press
			// away and the user can SEE the save happened without hunting
			// through the list.
			const auto it = std::find( s_ProfileNames.begin(), s_ProfileNames.end(), *oSanitized );
			if ( it != s_ProfileNames.end() )
				s_nSelectedProfile = (int)( it - s_ProfileNames.begin() );

			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Ok );
		}

		// Copies another game's *entire* config into this game's own
		// per-game file -- same full-snapshot, one-time-copy semantics as
		// EnableOverride()/UseProfile() above, and implicitly turns separate
		// settings on for this game too (there's no other file for a
		// per-game copy to land in). Synchronous for the same reason as
		// SaveCurrentAsNewProfile(): RefreshLists() right after needs the
		// file to exist.
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
				s_sStatus = "Couldn't read app " + sSourceId + "'s settings.";
				gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Error );
				return;
			}

			config::SnapshotPerGameOverride( *s_oAppId, *oSource );
			config::SetSessionOverrideActive( true );
			config::BumpConfigGeneration();
			s_bOverrideActive = true;
			s_sStatus = "Copied app " + sSourceId + "'s settings to this game.";
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Ok );
			RefreshLists();
		}

		// ---- Phase B actions (requests-2026-09-05 item 3) ------------------

		// "Save changes to profile": the manual half of auto-save. Writes
		// what the user is running right now into the active profile,
		// replacing it. Synchronous, like every other button here.
		void SaveChangesToActiveProfile()
		{
			const std::string sActive = config::ActiveProfile();
			if ( sActive.empty() )
				return;

			const bool bOk = config::SaveProfile( sActive, config::CurrentRoutedSettings() );
			s_sStatus = bOk
				? "Saved your changes into profile '" + sActive + "'."
				: "Couldn't write profile '" + sActive + "'.";
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Ok : gamescope::Notifications::Kind::Error );
			RefreshLists();
		}

		// "Rename profile": the SELECTED profile (the picker) takes the name
		// typed in the Manage group's Text row. config::RenameProfile() does
		// the containment checks and moves the active-profile name along if
		// it was the one renamed; the picker follows because RefreshLists()
		// re-lists and the selection is re-pointed at the new name below.
		void RenameSelectedProfile()
		{
			if ( s_nSelectedProfile < 0 || s_nSelectedProfile >= (int)s_ProfileNames.size() )
				return;
			const std::string sOld = s_ProfileNames[ s_nSelectedProfile ];

			std::optional<std::string> oNew = config::SanitizeProfileName( s_szRenameTo );
			if ( !oNew )
			{
				s_sStatus = "New name can't be empty (letters/digits/space/-/_ only).";
				gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Warning );
				return;
			}

			const bool bOk = config::RenameProfile( sOld, *oNew );
			s_sStatus = bOk
				? "Renamed profile '" + sOld + "' to '" + *oNew + "'."
				: "Couldn't rename '" + sOld + "' to '" + *oNew + "' -- does a profile with that name exist?";
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Ok : gamescope::Notifications::Kind::Error );
			if ( !bOk )
				return;

			s_szRenameTo[ 0 ] = '\0';
			if ( s_sLastAppliedProfile == sOld )
				s_sLastAppliedProfile = *oNew; // the file's breadcrumb still says the old name; the Status row should not
			RefreshLists();
			const auto it = std::find( s_ProfileNames.begin(), s_ProfileNames.end(), *oNew );
			if ( it != s_ProfileNames.end() )
				s_nSelectedProfile = (int)( it - s_ProfileNames.begin() );
		}

		// "Delete profile": the second destructive action in the product,
		// armed by Confirm() like the first. Only ever the SELECTED profile,
		// and config::DeleteProfile() refuses anything outside profiles/.
		// Running settings are untouched -- a profile is a saved copy, and
		// deleting the copy does not change what was copied from it.
		void DeleteSelectedProfile()
		{
			if ( s_nSelectedProfile < 0 || s_nSelectedProfile >= (int)s_ProfileNames.size() )
				return;
			const std::string sName = s_ProfileNames[ s_nSelectedProfile ];

			const bool bOk = config::DeleteProfile( sName );
			s_sStatus = bOk
				? "Deleted profile '" + sName + "'. Your current settings are unchanged."
				: "Couldn't delete profile '" + sName + "'.";
			gamescope::Notifications::Show( s_sStatus,
				bOk ? gamescope::Notifications::Kind::Info : gamescope::Notifications::Kind::Error );
			RefreshLists();
		}

		void SetAutoSave( bool bOn )
		{
			config::SetAutoSaveProfile( bOn );
			const std::string &sActive = config::ActiveProfile();
			if ( bOn && !sActive.empty() )
			{
				// Turning it on means "keep the profile equal to my settings
				// from now on" -- so start by making that true, rather than
				// leaving any existing drift in place until the next edit.
				config::SaveProfile( sActive, config::CurrentRoutedSettings() );
				s_sStatus = "Auto-save on: '" + sActive + "' now follows every change you make.";
			}
			else
			{
				s_sStatus = bOn
					? "Auto-save on. It starts working once a profile is active."
					: "Auto-save off. Profiles only change when you press Save changes.";
			}
			gamescope::Notifications::Show( s_sStatus, gamescope::Notifications::Kind::Info );
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
	// EnableOverride/DisableOverride/UseProfile/... functions,
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
			return panelconfig::PerGameFilePath( s_oAppId );
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
		// rebuild mechanism Audio does (ui::Area::Rebuilds). The inputs are
		// listed in panelconfig::StatusInputs (PanelConfig.h); the two new
		// ones since item 3 are the last-applied profile (the Status rows)
		// and the backup's presence (the "Restore previous settings" row).
		uint64_t ConfigGenerationHash()
		{
			EnsureInitialized();

			panelconfig::StatusInputs in;
			in.oAppId = s_oAppId;
			in.bOverrideActive = s_bOverrideActive;
			in.bHasSavedPerGameConfig = s_bHasSavedPerGameConfig;
			in.vecProfileNames = s_ProfileNames;
			in.vecOtherGameIds = s_OtherGameIds;
			in.sLastAppliedProfile = s_sLastAppliedProfile;
			in.bHasBackup = s_oBackup.has_value();
			in.bBackupWasOverride = s_oBackup ? s_oBackup->bWasOverride : false;
			// Phase B. All three are in-memory reads: the active profile and
			// auto-save come from ConfigManager's global.json mirror, and the
			// dirty count is cached on its mutation sequence -- it only
			// re-diffs (one profile-file read) after something was written.
			// So this runs every frame the area is shown without touching
			// the disk, and still notices a slider moved in another area.
			in.sActiveProfile = config::ActiveProfile();
			in.bAutoSave = config::AutoSaveProfile();
			in.nDirtySections = config::ActiveProfileDirtySections().value_or( -1 );
			return panelconfig::StatusHash( in );
		}

		bool HasActiveProfile() { return !config::ActiveProfile().empty(); }

		void RebuildProfileOptions()
		{
			s_dqProfileText.clear();
			s_vecProfileOptions.clear();
			for ( size_t i = 0; i < s_ProfileNames.size(); ++i )
			{
				s_dqProfileText.emplace_back( s_ProfileNames[ i ] );
				s_vecProfileOptions.push_back( { (int)i, s_dqProfileText.back().c_str() } );
			}
			// The picker exists even before the first profile is saved --
			// as a disabled row that says so -- instead of vanishing. A row
			// that is not there cannot explain why, and the old "the whole
			// picker is missing until I restart" report was exactly the
			// first-profile case: the row was registered only when the list
			// was non-empty, and the list never refreshed (see the
			// file-level comment).
			if ( s_vecProfileOptions.empty() )
			{
				s_dqProfileText.emplace_back( "no profiles yet" );
				s_vecProfileOptions.push_back( { 0, s_dqProfileText.back().c_str() } );
			}

			s_dqGameText.clear();
			s_vecGameOptions.clear();
			for ( size_t i = 0; i < s_OtherGameIds.size(); ++i )
			{
				s_dqGameText.emplace_back( "app " + s_OtherGameIds[ i ] );
				s_vecGameOptions.push_back( { (int)i, s_dqGameText.back().c_str() } );
			}
		}

		bool HasProfiles() { return !s_ProfileNames.empty(); }
		bool CanRestore()  { return s_oBackup && panelconfig::BackupMatchesRouting( *s_oBackup, s_bOverrideActive ); }

		constexpr const char *kNoProfilesReason =
			"no profiles yet -- save one in the Profiles area first";
		constexpr const char *kRestoreRoutingReason =
			"the backup was taken for a different settings file -- switch \"Use separate "
			"settings for this game\" back the way it was when you used the profile, then restore";

		// ---- Per-game ---------------------------------------------------
		void BuildPerGameArea( ui::Area &a )
		{
			RebuildProfileOptions();

			// Status first: what this game is running with, before any
			// control that changes it. The bare "resolved app id" used to
			// sit at the bottom of Diagnostics; it is the first thing a
			// newcomer needs to know, so it leads here.
			a.Group( "Status" );
			a.Facts( "config.status", "This game",
				[]{
					if ( !s_oAppId )
						return std::string( "no game identified" );
					return s_bOverrideActive ? std::string( "own settings" ) : std::string( "shared settings" );
				} )
				.Help( "What this game is running with right now: which game was recognised, whether "
				       "it has its own settings, and which profile they came from." )
				.Keywords( "status game app id own settings profile saved on disk" )
				.Live( "game", []{
					return ui::Fact{ "game", panelconfig::GameFact( s_oAppId ) };
				} )
				.Live( "own", []{
					return ui::Fact{ "own settings", panelconfig::OwnSettingsFact( s_bOverrideActive ) };
				} )
				.Live( "profile", []{
					return ui::Fact{ "profile", panelconfig::ProfileFact( s_sLastAppliedProfile ) };
				} )
				.Live( "saved", []{
					return ui::Fact{ "saved settings on disk", s_bHasSavedPerGameConfig
						? ( "yes -- " + PerGameFilePath() ) : std::string( "no" ) };
				} )
				// Phase B: how far this game's running settings have drifted
				// from the active profile. Same fact, same wording as the
				// Profiles area's -- one question, one answer.
				.Live( "changes", []{
					return ui::Fact{ "changes since applied",
						panelconfig::ChangesFact( config::ActiveProfileDirtySections() ) };
				} );

			a.Group( "Settings for this game" );

			a.Switch( "config.override", "Use separate settings for this game",
				ui::AnyBind::Of<bool>(
					[]{ return s_bOverrideActive; },
					[]( bool bOn )
					{
						if ( bOn )
							EnableOverride();
						else
							DisableOverride();
					} ) )
				.Help( "On: this game gets its own copy of your settings, applied every time it starts; "
				       "changes you make while playing are saved to it. Off: this game uses the shared "
				       "settings again -- what you saved for it is kept." )
				.Default( false )
				.Keywords( "separate settings per game override routing config file app id snapshot" )
				.DisabledUnless( []{ return s_oAppId.has_value(); },
					"no game was identified for this session -- gamescope-ritz was launched without "
					"GS_RITZ_APPID or a Steam app-id, so there is no per-game file to write. Every "
					"other panel is writing to global.json." );

			// "This game -> profile X", said once, in one place. Before this
			// row the same thing took the switch above plus a trip to the
			// Profiles area, and nothing told the user those two steps were
			// one idea.
			a.Action( "config.start_from_profile", "Start from profile", "use",
				[]{ StartFromProfile(); } )
				.Help( "Copies a profile into this game's settings, switching separate settings on if "
				       "they were off. Later changes to the profile won't follow -- edit here, or use it "
				       "again. Your previous settings are kept as a backup." )
				.Keywords( "start from profile use apply per game preset copy" )
				.DisabledUnless( []{ return s_oAppId.has_value() && HasProfiles(); },
					"needs a recognised game and at least one saved profile -- save one in the "
					"Profiles area first" )
				.Param( "profile", "Profile",
					ui::AnyBind::Of<int>(
						[]{ return std::max( s_nStartFromProfile, 0 ); },
						[]( int n ) { s_nStartFromProfile = n; } ),
					s_vecProfileOptions.data(), s_vecProfileOptions.size() )
					.Help( "Which profile to copy into this game's settings." )
					.Default( 0 )
					.DisabledUnless( HasProfiles, kNoProfilesReason );

			// The backup Use/Start from profile took, when it was taken FOR
			// THIS GAME's file. A backup taken for global.json is restored
			// from the Profiles area, where it was made.
			if ( s_oBackup && s_oBackup->bWasOverride )
			{
				a.Action( "config.restore", "Restore previous settings", "restore",
					[]{ RestorePreviousSettings(); } )
					.Help( "Brings back the settings this game had before you last started it from a "
					       "profile. Available until you use another profile." )
					.Keywords( "restore undo previous settings backup profile" )
					.DisabledUnless( CanRestore, kRestoreRoutingReason );
			}

			// Copying another game's config. The SOURCE is a parameter
			// rather than a second row: SPEC §5.3's whole argument is that
			// an action's operand is depth, and the sheet keeps the verb.
			if ( !s_OtherGameIds.empty() )
			{
				a.Action( "config.copy", "Copy another game's settings", "copy",
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
				a.Action( "config.delete", "Delete saved settings", "delete...",
					[]{ DeleteSavedPerGameConfig(); } )
					.Confirm( "delete permanently?" )
					.Help( "Deletes this game's saved settings for good -- this can't be undone. Press "
					       "once to arm the button, then again to confirm." )
					.Keywords( "delete remove destroy per game config file" );
			}

			a.Group( "Diagnostics" );
			a.Facts( "config.routing", "Using",
				[]{ return s_bOverrideActive ? std::string( "this game's file" ) : std::string( "global" ); } )
				.Help( "Which settings file this game is reading from, and which one is set aside." )
				.Keywords( "routing resolution using file provenance layers" )
				.Live( "file", []{
					return ui::Fact{ "settings file", s_bOverrideActive
						? ( PerGameFilePath() + "  (this game's own)" )
						: std::string( "global.json  (shared)" ) };
				} )
				.Live( "aside", []{
					return ui::Fact{ "set aside", s_bOverrideActive
						? std::string( "global.json -- this game's own settings win" )
						: ( s_bHasSavedPerGameConfig
							? ( PerGameFilePath() + " -- saved, switched off" )
							: std::string( "none" ) ) };
				} )
				.Live( "default", []{
					return ui::Fact{ "if a value is missing", "the built-in default is used" };
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

			// Status first: "what is in use" was the question the old area
			// never answered outright -- the last-applied profile was a
			// Diagnostics fact at the very bottom.
			// Phase B: the Status row reads the ACTIVE profile (Settings::
			// active_profile -- the one Use/Start from/Save as new last
			// chose), not the per-file provenance breadcrumb it read in
			// Phase A. The two agree except after a rename or delete, where
			// the file's breadcrumb still names the old profile and the
			// Status row must not.
			a.Group( "Status" );
			ui::Entry &status = a.Facts( "profiles.status", "In use",
				[]{
					const std::string &sActive = config::ActiveProfile();
					if ( sActive.empty() )
						return std::string( "no profile" );
					const std::optional<int> oDirty = config::ActiveProfileDirtySections();
					return ( oDirty && *oDirty > 0 ) ? sActive + "  ·  " + panelconfig::ChangesFact( oDirty ) : sActive;
				} )
				.Help( "Which profile you're working against, how far your settings have drifted from "
				       "it, where your edits are being saved, and whether the profile follows them." )
				.Keywords( "status in use active profile changes since applied dirty edits go to saving auto-save backup" )
				.Live( "profile", []{
					return ui::Fact{ "profile", HasActiveProfile()
						? config::ActiveProfile()
						: std::string( "none -- use a profile or save one to make it active" ) };
				} )
				.Live( "changes", []{
					return ui::Fact{ "changes since applied",
						panelconfig::ChangesFact( config::ActiveProfileDirtySections() ) };
				} )
				.Live( "target", []{
					return ui::Fact{ "edits go to", panelconfig::EditsGoTo( s_oAppId, s_bOverrideActive ) };
				} )
				.Live( "saving", []{
					return ui::Fact{ "saving",
						panelconfig::SavingFact( HasActiveProfile(), config::AutoSaveProfile() ) };
				} );
			if ( s_oBackup )
			{
				status.Live( "backup", []{
					return ui::Fact{ "backup", s_oBackup
						? ( "your settings from before '" + s_oBackup->sReplacedBy +
						    "' -- \"Restore previous settings\" brings them back" )
						: std::string( "none" ) };
				} );
			}

			a.Group( "Use a profile" );

			a.Choice( "profiles.list", "Profile",
				ui::AnyBind::Of<int>(
					[]{ return std::max( s_nSelectedProfile, 0 ); },
					[]( int n ) { s_nSelectedProfile = n; } ),
				s_vecProfileOptions.data(), s_vecProfileOptions.size() )
				.Help( "Which saved profile to use. A profile is a saved bundle of settings you can "
				       "come back to any time." )
				.Default( 0 )
				.Keywords( "profile preset saved snapshot pick" )
				.DisabledUnless( HasProfiles, kNoProfilesReason );

			// Phase B: confirm ONLY when there is something to lose -- edits
			// that drifted from the active profile with auto-save off. The
			// backup still makes Use reversible either way; the confirm is
			// for the case where the thing being replaced was never saved
			// anywhere but the live file. Decided at build time (the dirty
			// count and auto-save are both rebuild inputs), so a clean state
			// is a plain single press, as in Phase A.
			ui::Entry &apply = a.Action( "profiles.apply", "Use this profile", "use",
				[]{ UseSelectedProfile(); } )
				.Help( "Replaces your current settings with the selected profile's. The settings you "
				       "had are kept as a backup until you use another profile, so you can always go "
				       "back. If you have unsaved changes to the active profile, this asks first." )
				.Keywords( "use apply profile preset load switch" )
				.DisabledUnless( HasProfiles, kNoProfilesReason );
			{
				const std::string sPrompt = panelconfig::UseConfirmPrompt(
					config::ActiveProfileDirtySections(), config::AutoSaveProfile() );
				if ( !sPrompt.empty() )
					apply.Confirm( sPrompt.c_str() );
			}

			if ( s_oBackup )
			{
				a.Action( "profiles.restore", "Restore previous settings", "restore",
					[]{ RestorePreviousSettings(); } )
					.Help( "Brings back the settings you had before you last used a profile. Available "
					       "until you use another profile." )
					.Keywords( "restore undo previous settings backup profile" )
					.DisabledUnless( CanRestore, kRestoreRoutingReason );
			}

			a.Group( "Save" );

			a.Text( "profiles.name", "New profile name",
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

			a.Action( "profiles.save", "Save as new profile", "save",
				[]{ SaveCurrentAsNewProfile(); } )
				.Help( "Saves your current settings under a new name. It appears in the list above "
				       "right away." )
				.Keywords( "save profile new snapshot store" )
				.DisabledUnless( []{ return s_szNewProfileName[ 0 ] != '\0'; },
				                 "type a name first" );

			// ---- Phase B: the active profile's own save path ---------------
			// The disabled reason is picked at build time from the three
			// states the row can be in; the area rebuilds whenever any of
			// them changes (ConfigGenerationHash), so the reason is never
			// stale. No confirm: this overwrites a profile with the settings
			// the user is looking at, which is exactly what the button says.
			{
				ui::Entry &saveChanges = a.Action( "profiles.save_changes", "Save changes to profile", "save",
					[]{ SaveChangesToActiveProfile(); } )
					.Help( "Writes your current settings into the active profile, replacing what it had." )
					.Keywords( "save changes profile update overwrite active dirty" );
				const std::string sBlocker = panelconfig::SaveChangesBlocker(
					HasActiveProfile(), config::AutoSaveProfile(), config::ActiveProfileDirtySections() );
				if ( !sBlocker.empty() )
					saveChanges.DisabledUnless( []{ return false; }, sBlocker.c_str() );
			}

			a.Switch( "profiles.autosave", "Auto-save to profile",
				ui::AnyBind::Of<bool>(
					[]{ return config::AutoSaveProfile(); },
					[]( bool bOn ) { SetAutoSave( bOn ); } ) )
				.Help( "When on, any setting you change while a profile is active is saved into that "
				       "profile straight away. When off, changes stay in your current settings but the "
				       "profile keeps its old values until you press Save." )
				.Default( false )
				.Keywords( "auto-save autosave profile follow sync changes automatically" )
				.DisabledUnless( HasActiveProfile, "no profile is active" );

			// ---- Phase B: manage the SELECTED profile ----------------------
			// Rename and Delete act on the picker's selection (the same one
			// "Use this profile" would use), not on the active profile --
			// you should be able to tidy a profile you are not running.
			a.Group( "Manage selected profile" );

			a.Text( "profiles.rename_to", "New name",
				ui::AnyBind::Of<std::string>(
					[]{ return std::string( s_szRenameTo ); },
					[]( std::string s )
					{
						std::snprintf( s_szRenameTo, sizeof( s_szRenameTo ), "%s", s.c_str() );
					} ) )
				.Help( "The name the selected profile should have. Letters, digits, spaces, hyphens "
				       "and underscores only." )
				.Keywords( "rename profile new name" )
				.DisabledUnless( HasProfiles, kNoProfilesReason )
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

			a.Action( "profiles.rename", "Rename profile", "rename",
				[]{ RenameSelectedProfile(); } )
				.Help( "Gives the selected profile the name typed above. If it's the active profile, "
				       "everything follows the new name." )
				.Keywords( "rename profile move name" )
				.DisabledUnless( []{ return HasProfiles() && s_szRenameTo[ 0 ] != '\0'; },
				                 "select a profile and type its new name first" );

			// Armed by Confirm(), same as the per-game delete: the user's
			// rule is that nothing destroys a config on a single press.
			a.Action( "profiles.delete", "Delete profile", "delete...",
				[]{ DeleteSelectedProfile(); } )
				.Confirm( "delete permanently?" )
				.Help( "Deletes this saved profile. Settings you're running right now are not "
				       "affected. Press once to arm the button, then again to confirm." )
				.Keywords( "delete remove destroy profile file" )
				.DisabledUnless( HasProfiles, kNoProfilesReason );

			a.Group( "Diagnostics" );
			a.Facts( "profiles.facts", "Profiles",
				[]{ return std::to_string( s_ProfileNames.size() ) + " saved"; } )
				.Help( "Shows how many profiles you've saved and where they live on disk." )
				.Keywords( "profile count directory diagnostics last action" )
				.Live( "count", []{
					return ui::Fact{ "saved profiles", s_ProfileNames.empty()
						? "none yet" : std::to_string( s_ProfileNames.size() ) };
				} )
				.Live( "dir", []{
					return ui::Fact{ "profiles directory", config::ProfilesDir() };
				} )
				.Live( "status", []{
					return ui::Fact{ "last action", s_sStatus.empty() ? "none this session" : s_sStatus };
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
		profiles.Keywords( "profile preset snapshot save use apply restore backup named config" );
		profiles.Summary( []{
			EnsureInitialized();
			std::string sSummary = std::to_string( s_ProfileNames.size() ) + " saved";
			const std::string &sActive = config::ActiveProfile();
			if ( !sActive.empty() )
			{
				sSummary += "  ·  using " + sActive;
				const std::optional<int> oDirty = config::ActiveProfileDirtySections();
				if ( oDirty && *oDirty > 0 )
					sSummary += " (" + panelconfig::ChangesFact( oDirty ) + ")";
			}
			return sSummary;
		} );
		profiles.Badge( RoutedBadge );
		profiles.Rebuilds( ConfigGenerationHash, BuildProfilesArea );

		// ---- Per-game ------------------------------------------------
		ui::Area &pergame = reg.Add( "setup.pergame", "Per-game", ui::Section::Setup );
		pergame.Keywords( "per game separate settings override routing config file app id start from profile copy delete" );
		pergame.Summary( []{
			EnsureInitialized();
			if ( !s_oAppId )
				return std::string( "no game identified -- everything writes to global.json" );
			return s_bOverrideActive
				? ( "own settings  ·  " + PerGameFilePath() )
				: std::string( "shared settings (global)" );
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
