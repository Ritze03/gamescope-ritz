// The "Cursor" tab -- see PanelCursor.h for what this is. Every row below
// persists to global.json and is read every draw by Overlay/CursorArt.cpp's
// CursorArt_Draw() via GetCursorAppearance(), so moving a control here
// changes the overlay's pointer immediately -- see PanelCursor.h's
// GetCursorAppearance() comment for the wiring.
//
// Modelled directly on PanelDisplay.cpp's General area (a small, global-
// only settings group with no live gamescope state behind it) and on
// PanelConfig.cpp's Appearance area for the "process-level, global.json
// only" load/save shape -- `overlay.*` fields are never routed through
// config::EnqueueRoutedWrite()/ResolveEffective(SessionAppId()) the way a
// per-game-eligible field would be; see ConfigSchema.h's OverlaySettings
// comment for why (a per-game config file never even carries an `overlay`
// object -- ConfigManager.cpp's SettingsToJson(), bIncludeOverlay). So this
// file always reads via config::LoadGlobal() and always writes via
// config::EnqueueGlobalWrite(), exactly like PanelConfig.cpp's
// EnsureGeneralSettingsLoaded()/QueueGeneralSave() do for accent_hue and
// its neighbours -- copied rather than shared because that pair is static
// to PanelConfig.cpp's own translation unit, not exported.
//
// The outline colour's "follow accent / custom" shape is copied from
// FpsDisplay.cpp's RegisterModuleColor() (system.monitor's per-module
// colour overrides): a Composite(Color) bound to a packed 0xRRGGBB int that
// reads/writes CursorAppearance's current colour, with a `custom` Param
// that flips config::OverlaySettings::cursor_outline_color between
// std::nullopt (follow) and a captured explicit value. See that function's
// own comment for why the band declares no Default() of its own -- only
// `custom`'s -- and why: a captured default would go stale the moment the
// accent hue moves.
#include "PanelCursor.h"

#include <cstdio>

#include "Config/ConfigManager.h"
#include "CursorArt.h"

namespace gamescope
{
	namespace
	{
		// Loaded once per process and refreshed on every write this tab
		// makes -- same shape as PanelConfig.cpp's s_GeneralSettings/
		// EnsureGeneralSettingsLoaded(), and safe for the same reason: no
		// profile apply, per-game override toggle, or config reload ever
		// touches `overlay` (ConfigManager.h's ApplyProfile() doc comment),
		// so nothing outside this file can make the cache stale.
		bool s_bConfigLoaded = false;
		config::Settings s_Settings;

		void EnsureConfigLoaded()
		{
			if ( s_bConfigLoaded )
				return;
			s_bConfigLoaded = true;
			s_Settings = config::LoadGlobal();
		}

		void QueueSave()
		{
			config::EnqueueGlobalWrite( s_Settings );
		}

		// Packs an ImU32-ish 0xAARRGGBB/accent value down to 0xRRGGBB, the
		// on-disk shape every colour field here uses (matches FpsDisplay.cpp's
		// own PackColorRgb(), copied rather than shared for the same reason
		// noted in the file comment above -- it is static there too).
		int PackRgb( uint32_t uArgbOrRgb )
		{
			return (int)( uArgbOrRgb & 0xFFFFFFu );
		}
	}

	CursorAppearance GetCursorAppearance()
	{
		EnsureConfigLoaded();
		const auto &o = s_Settings.overlay;

		CursorAppearance a;
		a.flScale = o.cursor_scale;
		a.flOutlineWidth = o.cursor_outline_width;
		a.bOutlineFollowsAccent = !o.cursor_outline_color.has_value();
		a.uOutlineRgb = o.cursor_outline_color.has_value()
			? (uint32_t)*o.cursor_outline_color
			: gamescope::overlay::CursorArt_AccentRgb();
		a.uInlayRgb = (uint32_t)o.cursor_inlay_color;
		return a;
	}

	void PanelCursor_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "setup.cursor", "Cursor", ui::Section::Setup );
		a.Keywords( "cursor pointer mouse arrow shape size outline colour color design" );
		a.Summary( []{
			EnsureConfigLoaded();
			char sz[ 64 ];
			std::snprintf( sz, sizeof( sz ), "%.2fx  ·  %s outline",
				s_Settings.overlay.cursor_scale,
				s_Settings.overlay.cursor_outline_color.has_value() ? "custom" : "accent" );
			return std::string( sz );
		} );
		// overlay.* is always global.json, even with a per-game override
		// active -- same routing rule, same badge wording, as
		// setup.appearance (PanelConfig.cpp).
		a.Badge( []{ return std::string( "global only" ); } );

		a.Group( "Shape" );

		a.Slider( "cursor.scale", "Size",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.overlay.cursor_scale; },
				[]( float f ) { EnsureConfigLoaded(); s_Settings.overlay.cursor_scale = f; QueueSave(); } ) )
			.Help( "How big the pointer is while the overlay is open. Turn it up if it's easy to "
			       "lose track of on a bright or busy screen, or down if it feels oversized." )
			.Range( 0.5f, 3.0f )
			.Step( 0.1f )
			.Unit( "x" )
			.Default( config::OverlaySettings{}.cursor_scale )
			.Keywords( "size scale big small pointer" );

		a.Slider( "cursor.outline_width", "Outline thickness",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.overlay.cursor_outline_width; },
				[]( float f ) { EnsureConfigLoaded(); s_Settings.overlay.cursor_outline_width = f; QueueSave(); } ) )
			.Help( "How thick the outline around the pointer is. A heavier outline stays visible "
			       "over bright or busy game content; a thinner one is less distracting." )
			.Range( 1.0f, 6.0f )
			.Step( 0.5f )
			.Unit( "px" )
			.Default( config::OverlaySettings{}.cursor_outline_width )
			.Keywords( "outline thickness stroke border width" );

		a.Group( "Colours" );

		a.Composite( "cursor.outline_color", "Outline colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]
				{
					EnsureConfigLoaded();
					return s_Settings.overlay.cursor_outline_color.has_value()
						? *s_Settings.overlay.cursor_outline_color
						: PackRgb( gamescope::overlay::CursorArt_AccentRgb() );
				},
				[]( int nPacked )
				{
					EnsureConfigLoaded();
					s_Settings.overlay.cursor_outline_color = nPacked & 0xFFFFFF;
					QueueSave();
				} ) )
			.Help( "Colour of the pointer's outline. Off follows the overlay's own accent colour "
			       "automatically; on locks it to the colour you pick below." )
			.Keywords( "outline colour color accent tint" )
			.Param( "custom", "Custom colour",
				ui::AnyBind::Of<bool>(
					[]{ EnsureConfigLoaded(); return s_Settings.overlay.cursor_outline_color.has_value(); },
					[]( bool bCustom )
					{
						EnsureConfigLoaded();
						s_Settings.overlay.cursor_outline_color = bCustom
							? std::optional<int>( PackRgb( gamescope::overlay::CursorArt_AccentRgb() ) )
							: std::nullopt;
						QueueSave();
					} ) )
				.Default( false )
				.Help( "Off matches the overlay's own accent colour, so the outline follows it "
				       "automatically. On locks it to the colour above." );

		a.Composite( "cursor.inlay_color", "Inlay colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.overlay.cursor_inlay_color; },
				[]( int nPacked )
				{
					EnsureConfigLoaded();
					s_Settings.overlay.cursor_inlay_color = nPacked & 0xFFFFFF;
					QueueSave();
				} ) )
			.Help( "Colour of the solid fill inside the pointer's outline." )
			.Default( config::OverlaySettings{}.cursor_inlay_color )
			.Keywords( "inlay fill colour color inside" );
	}
}
