// The Setup section's areas of the settings overlay -- Profiles and
// Appearance. See superdoc/features/profiles.md (Profiles v2; "The Profiles
// area" section is this file's UI) and superdoc/planning/profiles-concept.md.
//
// PROFILES AREA (2026-09-06, the user's own sketch): a list of every
// profile leading the sheet, four equal verbs under it (Create / Copy / Edit
// / Delete, each a modal), an Inherits dropdown for a game profile, the
// "Filter game profiles" switch, and one Status row at the bottom. There are
// NO load or save buttons anywhere: selecting a line IS loading it (and is
// the assignment this game remembers), and every edit is saved into it as it
// happens -- profiles-concept.md v2, decision 3.
//
// The decisions live in PanelConfig.h's pure helpers (labels, filter,
// validation, status text) so tests can hold them; this file draws and calls
// the config layer.
//
// Thread safety: drawn from SettingsOverlay_AddLayer() on the steamcompmgr
// thread, same as PanelDisplay.cpp/PanelShaders.cpp -- see PanelDisplay.cpp's
// file-level comment for the full argument. The CRUD calls are synchronous
// file writes from that thread, on purpose: a button press followed by a
// directory re-read is the 2026-09-05 "restart to see a new profile" lesson.
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
#include "UI/Controls.h"   // controls::DeferToRelease(), the modals' atoms
#include "UI/Colors.h"
#include "Notifications.h"
#include "Palette.h"
#include "Fonts.h"
#include "../SettingsOverlay.h"
#include "steamcompmgr.hpp"   // force_repaint()

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		// ---- Appearance: global.json's `overlay` ------------------------
		// Unlike the other panels, this keeps its own cached
		// config::Settings -- see EnsureGeneralSettingsLoaded()'s comment
		// for why these fields can't reuse the EnsureConfigLoaded()+
		// ResolvedSettings() pattern (overlay.* is global.json-only, never
		// routed into a profile).
		bool s_bGeneralSettingsLoaded = false;
		config::Settings s_GeneralSettings;

		// overlay.* is process-level/global.json-only (ConfigSchema.h's own
		// comment on OverlaySettings) -- deliberately config::LoadGlobal(),
		// never config::ResolvedSettings(): a profile never carries
		// `overlay`. Reloaded on every config generation bump like every
		// other panel (2026-09-07). The old "nothing outside this area can
		// make s_GeneralSettings stale" claim was false: the Cursor area
		// and the notification placement write the same `overlay` object
		// from their own copies, and this copy was written back whole --
		// one Cursor write undid nine of this area's fields, and this
		// area's next write undid the Cursor's (settings-audit 2026-09-07).
		// config::EnqueueGlobalWrite() now merges per field, so a stale
		// copy cannot clobber; the reload keeps the read side honest and
		// matches the merge's bookkeeping, which a bump clears.
		// LoadGlobal() serves the in-process mirror, so this is cheap.
		uint64_t s_ulGeneralLoadedGeneration = 0;
		void EnsureGeneralSettingsLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bGeneralSettingsLoaded && ulGeneration == s_ulGeneralLoadedGeneration )
				return;
			s_GeneralSettings = config::LoadGlobal();
			s_ulGeneralLoadedGeneration = ulGeneration;
			s_bGeneralSettingsLoaded = true;
		}

		// Pushes the fields Widgets.cpp/Shell.cpp read live (display scale,
		// window transparency) into gamescope::palette::g_LiveTheme,
		// notification_scale/
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
			live.flWindowOpacity = o.window_opacity;

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

		// overlay.* is process-level, never profile-routed (see
		// EnsureGeneralSettingsLoaded()'s comment) -- so this always writes
		// straight to global.json, unlike every other panel's QueueSave(),
		// which goes through config::EnqueueRoutedWrite() into the session
		// profile.
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

		// =================================================================
		//  Profiles
		// =================================================================
		// ---- the cached directory read ------------------------------------
		// ListProfiles() is blocking directory I/O, so it is read once per
		// change, never per frame: the config generation covers everything
		// the config layer does (select, CRUD, reset), s_nListDirty covers
		// what only this file knows about (the filter switch).
		struct ProfilesCache
		{
			bool                              bLoaded = false;
			uint64_t                          ulGeneration = 0;
			uint32_t                          nDirty = 0;
			std::vector<config::ProfileMeta>  all;
			std::vector<size_t>               visible;
			std::vector<ui::ListItem>         items;
			std::vector<std::string>          inheritNames;   // "None" + generals
		};
		// The Inherits dropdown's option labels. Refreshed ONLY inside
		// BuildProfilesArea(), never by EnsureProfilesLoaded(): an Entry's
		// Options keep pszLabel pointers into these strings, and a cache
		// refresh mid-frame (a setter's own re-read) would otherwise free
		// them under a Choice drawn later that same frame. The area rebuilds
		// on the same triggers the cache refreshes on, so the two cannot
		// drift for more than the frame in which the change happened.
		std::vector<std::string> s_InheritLabels;
		std::vector<ui::Option>  s_InheritOptions;
		ProfilesCache s_Profiles;
		uint32_t      s_nListDirty = 0;

		bool FilterOtherGames()
		{
			EnsureGeneralSettingsLoaded();
			return s_GeneralSettings.overlay.profiles_filter_other_games;
		}

		void EnsureProfilesLoaded()
		{
			const uint64_t ulGen = config::ConfigGeneration();
			if ( s_Profiles.bLoaded && s_Profiles.ulGeneration == ulGen && s_Profiles.nDirty == s_nListDirty )
				return;
			s_Profiles.bLoaded      = true;
			s_Profiles.ulGeneration = ulGen;
			s_Profiles.nDirty       = s_nListDirty;

			s_Profiles.all     = config::ListProfiles();
			s_Profiles.visible = panelconfig::VisibleProfiles( s_Profiles.all, FilterOtherGames(),
				config::SessionAppId(), config::SessionProfile() );

			const bool bOverride = config::SessionProfileOverride().has_value();
			s_Profiles.items.clear();
			for ( size_t i : s_Profiles.visible )
			{
				const config::ProfileMeta &m = s_Profiles.all[ i ];
				s_Profiles.items.push_back( ui::ListItem{ panelconfig::ListName( m ), panelconfig::ListTag( m ),
					panelconfig::ListSecondary( m, config::SessionProfile(), bOverride ) } );
			}

			s_Profiles.inheritNames = panelconfig::InheritOptionNames( s_Profiles.all );
		}

		// The session profile's metadata, from the cache; a profile the
		// directory read did not find (a race with an external delete) still
		// yields a usable general meta named after the session.
		config::ProfileMeta SessionMeta()
		{
			EnsureProfilesLoaded();
			for ( const config::ProfileMeta &m : s_Profiles.all )
				if ( m.name == config::SessionProfile() )
					return m;
			config::ProfileMeta m;
			m.name = config::SessionProfile();
			return m;
		}

		std::string SessionBadge()
		{
			return panelconfig::SessionBadge( SessionMeta(), config::SessionProfileOverride().has_value() );
		}

		// Rebuild input for the dynamic area: which rows exist (the
		// Inherits row only for a game profile) and what the dropdown offers
		// both follow the profile set, which the generation and the dirty
		// counter together track.
		uint64_t ProfilesHash()
		{
			return ( config::ConfigGeneration() + 1 ) * 1315423911ull ^ (uint64_t)s_nListDirty;
		}

		void Toast( const std::string &s, Notifications::Kind eKind = Notifications::Kind::Info )
		{
			Notifications::Show( s, eKind );
		}

		// Selecting a line: the one action the list has. Loads the profile
		// and remembers it for this game (config::SelectProfile), which is
		// why there is no Load button -- and clears a `--profile` override,
		// so selecting the override's own line turns it into the assignment.
		void SelectVisible( int nIndex )
		{
			EnsureProfilesLoaded();
			if ( nIndex < 0 || nIndex >= (int)s_Profiles.visible.size() )
				return;
			const std::string sName = s_Profiles.all[ s_Profiles.visible[ (size_t)nIndex ] ].name;
			if ( sName == config::SessionProfile() && !config::SessionProfileOverride() )
				return;
			if ( config::SelectProfile( sName ) )
			{
				Toast( "Now editing '" + sName + "'" );
				force_repaint();
			}
		}

		// ---- the modals -----------------------------------------------------
		// A modal body's rows are ordinary rows, so a LABEL is the same thing
		// a sheet row draws for itself via SplitLabelZone() -- one call, and
		// the modal gets the sheet's own label column.
		ui::RowCtx LabeledRow( ui::ModalBodyCtx &ctx, const char *pszLabel )
		{
			ui::RowCtx row = ui::ModalNextRow( ctx );
			ImRect rcLabel, rcValue;
			row.SplitLabelZone( 0.0f, &rcLabel, &rcValue );
			ui::DrawText( rcLabel, ui::TypeRole::Label, ui::Col( ui::Role::TextLabel ), pszLabel );
			return row;
		}

		// One line of prose in a modal body -- the Delete prompt, an error.
		void ModalLine( ui::ModalBodyCtx &ctx, const char *pszText, ui::Role eRole )
		{
			const ImRect rc = ui::ModalNextBlock( ctx, ui::Px( ui::tok::kControlH ) );
			ui::DrawText( rc, ui::TypeRole::Body, ui::Col( eRole ), pszText );
		}

		enum class FormKind { Create, Copy, Edit };

		// The Create / Copy / Edit form. One state, one body: the three
		// differ only in what they are prefilled with and which config call
		// the primary makes. Errors stay INLINE -- the modal stays open with
		// the typed fields intact (ModalSpec::fnValidate), never a toast.
		struct Form
		{
			FormKind    eKind = FormKind::Create;
			std::string sSubject;        // the profile Copy/Edit acts on
			bool        bGame = false;
			std::string sAppId, sName;
			bool        bEditingAppId = false, bEditingName = false;
			std::string sNameError, sAppIdError, sOpError;
			std::string sCreatedName;    // set by fnValidate for fnPrimary
		};
		Form s_Form;

		float FormRows()
		{
			float fl = s_Form.bGame ? 3.0f : 2.0f;
			if ( !s_Form.sNameError.empty() ) fl += 1.0f;
			if ( !s_Form.sAppIdError.empty() ) fl += 1.0f;
			if ( !s_Form.sOpError.empty() ) fl += 1.0f;
			return fl;
		}

		void DrawFormBody( ui::ModalBodyCtx &ctx )
		{
			Form &f = s_Form;
			ui::controls::Switch( LabeledRow( ctx, "Game specific" ), "gamespecific", &f.bGame );
			if ( f.bGame )
			{
				ui::controls::Text( LabeledRow( ctx, "GameID" ), "gameid", &f.sAppId, &f.bEditingAppId,
				                    "app id", f.sAppIdError.empty() ? nullptr : f.sAppIdError.c_str() );
				if ( !f.sAppIdError.empty() )
					ModalLine( ctx, f.sAppIdError.c_str(), ui::Role::WarnText );
			}
			ui::controls::Text( LabeledRow( ctx, f.eKind == FormKind::Edit ? "Profile name" : "New profile name" ),
			                    "name", &f.sName, &f.bEditingName, "name",
			                    f.sNameError.empty() ? nullptr : f.sNameError.c_str() );
			if ( !f.sNameError.empty() )
				ModalLine( ctx, f.sNameError.c_str(), ui::Role::WarnText );
			if ( !f.sOpError.empty() )
				ModalLine( ctx, f.sOpError.c_str(), ui::Role::WarnText );
		}

		// The primary press: check the fields, then ask the config layer.
		// Either refusal is written into the form and keeps the modal open.
		bool FormValidate()
		{
			Form &f = s_Form;
			EnsureProfilesLoaded();
			const panelconfig::FormCheck check = panelconfig::CheckProfileForm(
				f.bGame, f.sAppId, f.sName, s_Profiles.all,
				f.eKind == FormKind::Edit ? f.sSubject : std::string() );
			f.sNameError  = check.sNameError;
			f.sAppIdError = check.sAppIdError;
			f.sOpError.clear();
			if ( !check.ok() )
				return false;

			const config::ProfileMeta session = SessionMeta();
			config::ProfileMeta meta;
			if ( f.eKind == FormKind::Edit )
			{
				for ( const config::ProfileMeta &m : s_Profiles.all )
					if ( m.name == f.sSubject )
						meta = m;
			}
			meta.name = check.sName;
			meta.kind = f.bGame ? config::ProfileKind::Game : config::ProfileKind::General;
			if ( f.bGame )
			{
				meta.app_id = f.sAppId;
				// The running game's title, when the id is the running game's.
				if ( config::SessionAppId() && *config::SessionAppId() == f.sAppId &&
				     config::SessionGameName() != f.sAppId )
					meta.game_name = config::SessionGameName();
				if ( f.eKind != FormKind::Edit )
				{
					// A new child of the general profile in play (Create), or
					// of what the source inherits (Copy of a game profile) /
					// the source itself (Copy of a general one).
					if ( f.eKind == FormKind::Create )
						meta.inherits = panelconfig::NewGameInherits( session );
					else
					{
						for ( const config::ProfileMeta &m : s_Profiles.all )
							if ( m.name == f.sSubject )
								meta.inherits = m.kind == config::ProfileKind::Game ? m.inherits : m.name;
					}
					if ( meta.inherits == meta.name )
						meta.inherits.clear();
				}
			}
			else
			{
				meta.app_id.clear();
				meta.game_name.clear();
				meta.inherits.clear();
			}

			config::ProfileOp op;
			switch ( f.eKind )
			{
				case FormKind::Create: op = config::CreateProfile( meta ); break;
				case FormKind::Copy:   op = config::CopyProfile( f.sSubject, meta ); break;
				case FormKind::Edit:   op = config::EditProfileMeta( f.sSubject, meta ); break;
			}
			if ( !op )
			{
				f.sOpError = op.error;
				return false;
			}
			f.sCreatedName = meta.name;
			return true;
		}

		void FormPrimary()
		{
			const Form f = s_Form;
			s_nListDirty++;
			switch ( f.eKind )
			{
				case FormKind::Create:
				case FormKind::Copy:
					// A new profile is selected straight away -- the user
					// made it to use it -- with the one toast that says so.
					config::SelectProfile( f.sCreatedName );
					Toast( ( f.eKind == FormKind::Create ? "Created '" : "Copied to '" ) + f.sCreatedName + "'" );
					break;
				case FormKind::Edit:
					Toast( "Saved '" + f.sCreatedName + "'" );
					break;
			}
			force_repaint();
		}

		void OpenForm( FormKind eKind )
		{
			if ( ui::IsModalOpen() )
				return;
			EnsureProfilesLoaded();
			const config::ProfileMeta session = SessionMeta();
			const std::optional<std::string> &oAppId = config::SessionAppId();

			s_Form = Form{};
			s_Form.eKind    = eKind;
			s_Form.sSubject = session.name;
			switch ( eKind )
			{
				case FormKind::Create:
					s_Form.bGame  = false;
					s_Form.sAppId = oAppId ? *oAppId : std::string();
					break;
				case FormKind::Copy:
					s_Form.bGame  = session.kind == config::ProfileKind::Game;
					s_Form.sAppId = !session.app_id.empty() ? session.app_id : ( oAppId ? *oAppId : std::string() );
					break;
				case FormKind::Edit:
					s_Form.bGame  = session.kind == config::ProfileKind::Game;
					s_Form.sAppId = !session.app_id.empty() ? session.app_id : ( oAppId ? *oAppId : std::string() );
					s_Form.sName  = session.name;
					break;
			}

			ui::ModalSpec spec;
			switch ( eKind )
			{
				case FormKind::Create: spec.sTitle = "Create profile";           spec.sPrimaryLabel = "Create"; break;
				case FormKind::Copy:   spec.sTitle = "Copy '" + session.name + "'"; spec.sPrimaryLabel = "Copy";   break;
				case FormKind::Edit:   spec.sTitle = "Edit '" + session.name + "'"; spec.sPrimaryLabel = "Save";   break;
			}
			spec.flMinBodyRows = FormRows();
			spec.fnBody     = DrawFormBody;
			spec.fnValidate = FormValidate;
			spec.fnPrimary  = FormPrimary;
			ui::OpenModal( std::move( spec ) );
		}

		// Delete: a question, the consequence for its children, and a
		// danger-tinted primary. The last profile's Delete verb is disabled
		// before this is ever reached (DeleteBlocker), so the modal never
		// has to refuse for that reason; a config-layer refusal is still
		// shown inline.
		std::string s_sDeleteError;

		void OpenDelete()
		{
			if ( ui::IsModalOpen() )
				return;
			EnsureProfilesLoaded();
			const config::ProfileMeta session = SessionMeta();
			const size_t nChildren = panelconfig::CountChildren( s_Profiles.all, session.name );
			s_sDeleteError.clear();

			ui::ModalSpec spec;
			spec.sTitle         = "Delete '" + session.name + "'?";
			spec.sPrimaryLabel  = "Delete";
			spec.bPrimaryDanger = true;
			spec.flMinBodyRows  = nChildren ? 2.0f : 1.0f;
			const std::string sChildren = panelconfig::DeleteChildrenLine( nChildren );
			spec.fnBody = [ sChildren ]( ui::ModalBodyCtx &ctx )
			{
				ModalLine( ctx, "Are you sure?", ui::Role::TextBody );
				if ( !sChildren.empty() )
					ModalLine( ctx, sChildren.c_str(), ui::Role::TextMeta );
				if ( !s_sDeleteError.empty() )
					ModalLine( ctx, s_sDeleteError.c_str(), ui::Role::WarnText );
			};
			const std::string sName = session.name;
			spec.fnValidate = [ sName ]
			{
				const config::ProfileOp op = config::DeleteProfile( sName );
				if ( !op )
				{
					s_sDeleteError = op.error;
					return false;
				}
				return true;
			};
			spec.fnPrimary = [ sName ]
			{
				s_nListDirty++;
				Toast( "Deleted '" + sName + "'" );
				force_repaint();
			};
			ui::OpenModal( std::move( spec ) );
		}

		std::string DeleteReason()
		{
			EnsureProfilesLoaded();
			return panelconfig::DeleteBlocker( s_Profiles.all.size() );
		}

		// ---- the Inherits dropdown --------------------------------------------
		void SetInherits( int nOption )
		{
			EnsureProfilesLoaded();
			config::ProfileMeta meta = SessionMeta();
			if ( meta.kind != config::ProfileKind::Game )
				return;
			if ( nOption < 0 || nOption >= (int)s_Profiles.inheritNames.size() )
				return;
			const std::string sParent = nOption == 0 ? std::string() : s_Profiles.inheritNames[ (size_t)nOption ];
			if ( sParent == meta.inherits )
				return;
			meta.inherits = sParent;
			const config::ProfileOp op = config::EditProfileMeta( meta.name, meta );
			if ( !op )
			{
				// The one Profiles control that is not a modal; the refusal
				// still has to be seen somewhere, and this row has no field
				// to sit beside.
				Toast( op.error, Notifications::Kind::Error );
				return;
			}
			s_nListDirty++;
			force_repaint();
		}

		// ---- the area ----------------------------------------------------------
		void BuildProfilesArea( ui::Area &a )
		{
			EnsureProfilesLoaded();
			const config::ProfileMeta session = SessionMeta();

			// See s_InheritLabels: the labels an Entry points at live here,
			// and only a rebuild -- which frees the old Entry first -- may
			// replace them.
			s_InheritLabels = s_Profiles.inheritNames;
			s_InheritOptions.clear();
			for ( size_t i = 0; i < s_InheritLabels.size(); ++i )
				s_InheritOptions.push_back( ui::Option{ (int)i, s_InheritLabels[ i ].c_str() } );

			a.Group( "Profiles" );

			a.Composite( "profiles.list", "Profiles", ui::CompositeKind::List,
				ui::AnyBind::Of<int>(
					[]{
						EnsureProfilesLoaded();
						return panelconfig::VisibleIndexOf( s_Profiles.all, s_Profiles.visible,
							config::SessionProfile() );
					},
					[]( int n ) { SelectVisible( n ); } ) )
				.Items( []{ EnsureProfilesLoaded(); return s_Profiles.items; } )
				.ListAction( "Create", []{ OpenForm( FormKind::Create ); } )
				.ListAction( "Copy",   []{ OpenForm( FormKind::Copy ); } )
				.ListAction( "Edit",   []{ OpenForm( FormKind::Edit ); } )
				.ListAction( "Delete", OpenDelete, /* bDanger */ true, DeleteReason )
				.Help( "Every saved profile. Selecting one loads it and makes it this game's profile -- "
				       "every change you make afterwards is saved straight into it. Game profiles are "
				       "marked [Game] and may inherit a general profile's values. Create, Copy, Edit and "
				       "Delete act on the selected profile." )
				.Keywords( "profile profiles list select load save use apply restore per-game this game "
				           "override game general create copy edit delete rename inherit inherits" )
				.Live( "count", []{
					EnsureProfilesLoaded();
					return ui::Fact{ "profiles", std::to_string( s_Profiles.all.size() ) + " on disk, " +
						std::to_string( s_Profiles.visible.size() ) + " shown" };
				} )
				.Live( "file", []{
					return ui::Fact{ "file", config::ProfilePath( config::SessionProfile() ) };
				} )
				.Live( "dir", []{
					return ui::Fact{ "profiles directory", config::ProfilesDir() };
				} );

			if ( session.kind == config::ProfileKind::Game )
			{
				a.Choice( "profiles.inherits", "Inherits",
					ui::AnyBind::Of<int>(
						[]{
							EnsureProfilesLoaded();
							return panelconfig::InheritIndex( s_Profiles.inheritNames, SessionMeta().inherits );
						},
						SetInherits ),
					s_InheritOptions.data(), s_InheritOptions.size() )
					// Dropdown, not segmented (2026-09-06, user feedback: "The
					// inheritance selector should be a dropdown. Not multiple
					// buttons."). The option set is every saved general
					// profile -- user-created and unbounded, not a fixed
					// handful of words -- exactly ui-design-guide.md's
					// Dropdown-vs-segmented rule for when to force this.
					.Dropdown()
					.Help( "The general profile this game profile takes its values from. Only what you "
					       "change here is stored in this profile; everything else follows the parent as "
					       "it changes. None makes it stand alone with a full copy of the values." )
					.Keywords( "inherits inherit parent base general profile" );
			}

			a.Switch( "profiles.filter", "Filter game profiles",
				ui::AnyBind::Of<bool>(
					[]{ return FilterOtherGames(); },
					[]( bool b )
					{
						EnsureGeneralSettingsLoaded();
						s_GeneralSettings.overlay.profiles_filter_other_games = b;
						QueueGeneralSave();
						s_nListDirty++;
					} ) )
				.Help( "On, the list shows only general profiles and this game's own. Off, it shows "
				       "every game's profiles too." )
				.Key( "overlay.profiles_filter_other_games" )
				.Default( config::OverlaySettings{}.profiles_filter_other_games )
				.Keywords( "filter game profiles show hide other games list" );

			a.Group( "Status" );
			a.Facts( "profiles.status", "Status",
				[]{
					return panelconfig::StatusSummary( SessionMeta(), config::SessionGameName(),
						config::SessionAppId(), config::SessionProfileOverride() );
				} )
				.Help( "Which profile every change is saved into right now, what it inherits, and "
				       "which game this session belongs to." )
				.Keywords( "status editing session inherits launch option game" )
				.Live( "summary", []{
					return ui::Fact{ "status", panelconfig::StatusLong( SessionMeta(), config::SessionGameName(),
						config::SessionAppId(), config::SessionProfileOverride() ) };
				} )
				.Live( "editing", []{
					return ui::Fact{ "editing", panelconfig::ListLabel( SessionMeta() ) + " (" + config::SessionProfile() + ".json)" };
				} )
				.Live( "inherits", []{
					const std::optional<std::string> oParent = config::SessionProfileParent();
					return ui::Fact{ "inherits", oParent ? *oParent : std::string( "nothing -- a standalone profile" ) };
				} )
				.Live( "launch", []{
					const std::optional<std::string> &oOverride = config::SessionProfileOverride();
					return ui::Fact{ "launch option", oOverride ? *oOverride + " (this session only)" : std::string( "none" ) };
				} )
				.Live( "game", []{
					return ui::Fact{ "game", panelconfig::GameStatusFact( config::SessionGameName(), config::SessionAppId() ) };
				} );
		}

		// ---- Appearance -------------------------------------------------
		// Every row here writes global.json unconditionally, which is why
		// this area's badge reads "global only" whatever profile the session
		// edits. That routing rule predates E2 (see
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
				.Key( "overlay.accent_hue" )
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

			// requests-2026-09-06.md item 2: the three sliders that used to
			// live here (Window focused/unfocused, Dock) wrote
			// OverlaySettings::opacity_windows_focused/unfocused/opacity_dock
			// -- fields Chrome.cpp's multi-window era read and P5's dock/
			// panel-window deletion left with no reader at all (see
			// ConfigSchema.h and Palette.h's own removal notes). Deleted
			// rather than left dormant, same as dock_scale before them. This
			// one slider replaces all three: it is wired to something that
			// actually draws now -- Shell.cpp's slab background and
			// Inspector fill, via palette::WindowOpacity().
			a.Slider( "overlay.window_opacity", "Window transparency",
				BindOverlayFloat( &config::OverlaySettings::window_opacity ) )
				.Help( "How see-through the settings window is. Lower it to see more of the game "
				       "behind it." )
				.Range( 0.3f, 1.0f )
				.Step( 0.05f )       // 15 positions; every alpha default is on the grid
				.Default( config::OverlaySettings{}.window_opacity )
				.Keywords( "opacity transparency window alpha see-through backdrop" );

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
			// s_GeneralSettings -- the stale-cache clobber that
			// EnqueueGlobalWrite()'s per-field merge closed on 2026-09-07
			// (see EnsureGeneralSettingsLoaded()'s comment).
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

			// requests-2026-09-07 item 10: this used to be ONE Facts row
			// bundling three genuinely different topics (write routing, the
			// font atlas' live bake scale, and the on-disk config path)
			// under a single generic "Appearance" title. Split into three
			// named rows -- the same convention PanelChangelog.cpp already
			// uses for its own three Facts rows (gamescope / gamescope-ritz
			// / Changelog) -- so each summary line says what it is about
			// instead of three unrelated facts sharing one label. No new
			// information: same three Live() facts as before, just given
			// their own row and title apiece.
			//
			// This also happens to be why the area regressed to one column
			// during the same day's transparency work (`6d62695`): the
			// column ladder is driven purely by row COUNT (Layout.cpp's
			// Solve(), `ceil(EntryCount / kRowsPerColumn)`, kRowsPerColumn
			// == 12 -- there is no per-area column override), and that
			// commit's three-sliders-into-one simplification dropped this
			// area from 13 rows to 11, crossing under the 12-row threshold
			// for a second column. Splitting this one Facts row into three
			// restores 13 -- the fix earns its own keep on clarity grounds
			// above, and the column count follows from it rather than the
			// other way around.
			a.Facts( "overlay.appearance_routing_facts", "Routing",
				[]{
					return std::string( "global.json always" );
				} )
				.Help( "Shows where these appearance settings are saved." )
				.Keywords( "appearance diagnostics global routing" )
				.Live( "routing", []{
					return ui::Fact{ "written to",
						"global.json always -- overlay appearance is process-level, so a per-game "
						"override does not apply to it" };
				} );

			a.Facts( "overlay.appearance_atlas_facts", "Font atlas",
				[]{
					char sz[ 48 ];
					std::snprintf( sz, sizeof( sz ), "%.2fx", gamescope::fonts::BuiltScale() );
					return std::string( sz );
				} )
				.Help( "Shows the scale the overlay's font atlas is currently baked at." )
				.Keywords( "appearance diagnostics atlas font scale" )
				.Live( "atlas", []{
					char sz[ 48 ];
					std::snprintf( sz, sizeof( sz ), "%.2fx", gamescope::fonts::BuiltScale() );
					return ui::Fact{ "font atlas baked at", sz };
				} );

			a.Facts( "overlay.appearance_root_facts", "Config location",
				[]{
					return config::ConfigRoot();
				} )
				.Help( "Shows the directory these appearance settings are read from and written to." )
				.Keywords( "appearance diagnostics config directory root path" )
				.Live( "root", []{
					return ui::Fact{ "config directory", config::ConfigRoot() };
				} );
		}
	}

	void PanelConfig_RegisterAreas( ui::Registry &reg )
	{
		// ---- the session, registry-wide ---------------------------------
		// Every area's badge is the session profile unless it declares its
		// own (Appearance below), and every row's inherited / overridden
		// marker is answered here -- the one place that knows profiles.
		reg.DefaultBadge( SessionBadge );
		reg.Inheritance(
			[]{
				const std::optional<std::string> oParent = config::SessionProfileParent();
				return oParent ? *oParent : std::string();
			},
			[]( const std::string &sKey )
			{
				if ( !config::SessionProfileParent() || !config::IsSettingsKey( sKey ) )
					return ui::InheritState::Plain;
				return config::OverriddenKeys().count( sKey )
					? ui::InheritState::Overridden : ui::InheritState::Inherited;
			},
			[]( const std::string &sKey )
			{
				const bool bOk = config::ResetKeyToInherited( sKey );
				if ( bOk )
					force_repaint();
				return bOk;
			} );

		// ---- Profiles ------------------------------------------------
		ui::Area &profiles = reg.Add( "setup.profiles", "Profiles", ui::Section::Setup );
		profiles.Keywords( "profile preset game general inherits create copy edit delete named config per-game" );
		profiles.Summary( []{ return "editing " + SessionBadge(); } );
		profiles.Rebuilds( ProfilesHash, BuildProfilesArea );

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
		// Always global, whatever profile the session edits -- the one
		// routing rule the session cannot express.
		appearance.Badge( []{ return std::string( "global only" ); } );
		BuildAppearanceArea( appearance );
	}
}
