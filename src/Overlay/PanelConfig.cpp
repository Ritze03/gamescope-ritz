// The Setup section's areas of the settings overlay -- Profiles and
// Appearance. See superdoc/features/profiles.md and
// superdoc/planning/profiles-concept.md (v2).
//
// PROFILES AREA: BEING REBUILT (2026-09-06). The v1 rows (Use / Restore /
// Save as new / Save changes / Auto-save / Rename / Delete, and the whole
// Per-game area) were removed with the model they served; the invisible
// layer underneath is done (Config/ConfigManager.h: SessionProfile(),
// SelectProfile(), CreateProfile()/CopyProfile()/EditProfileMeta()/
// DeleteProfile(), ListProfiles(), OverriddenKeys(), ResetKeyToInherited())
// and the list-with-modals UI the user chose lands in a follow-up. Until
// then the area is one honest Facts row saying what the session is editing.
//
// Thread safety: drawn from SettingsOverlay_AddLayer() on the steamcompmgr
// thread, same as PanelDisplay.cpp/PanelShaders.cpp -- see PanelDisplay.cpp's
// file-level comment for the full argument.
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
		// `overlay`. Loaded once per process, matching every other panel's
		// "cache locally, push on every edit" shape -- and unlike those
		// panels, this one never needs a config::ConfigGeneration() reload
		// check: switching profiles never touches `overlay`, so nothing
		// outside this area itself can ever make s_GeneralSettings stale.
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

		// ---- Profiles (placeholder while the area is rebuilt) ----------
		std::string SessionBadge()
		{
			const std::string &sProfile = config::SessionProfile();
			return config::SessionProfileOverride() ? sProfile + " (launch option)" : sProfile;
		}

		uint64_t ProfilesHash()
		{
			// The placeholder shows only what the session resolves to, so
			// the config generation is the whole rebuild input.
			return config::ConfigGeneration() + 1;
		}

		void BuildProfilesArea( ui::Area &a )
		{
			a.Group( "Status" );
			a.Facts( "profiles.status", "Being rebuilt",
				[]{ return SessionBadge(); } )
				.Help( "The Profiles area is being rebuilt around a list of profiles with Create, Copy, "
				       "Edit and Delete. Until then this shows which profile every change is saved into." )
				.Keywords( "profile status session editing inherits launch option game" )
				.Live( "editing", []{
					return ui::Fact{ "editing", config::SessionProfile() + " -- every change is saved into it" };
				} )
				.Live( "kind", []{
					const std::optional<std::string> oParent = config::SessionProfileParent();
					return ui::Fact{ "inherits", oParent ? *oParent : std::string( "nothing -- a standalone profile" ) };
				} )
				.Live( "launch", []{
					const std::optional<std::string> &oOverride = config::SessionProfileOverride();
					return ui::Fact{ "launch option", oOverride ? *oOverride + " (this session only)" : std::string( "none" ) };
				} )
				.Live( "game", []{
					return ui::Fact{ "game", panelconfig::GameFact( config::SessionAppId() ) };
				} )
				.Live( "dir", []{
					return ui::Fact{ "profiles directory", config::ProfilesDir() };
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
		profiles.Keywords( "profile preset game general inherits create copy edit delete named config" );
		profiles.Summary( []{ return "editing " + SessionBadge(); } );
		profiles.Badge( SessionBadge );
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
