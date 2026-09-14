// The zoom -- see Zoom.h for the split between this file and the render
// side, and superdoc/features/zoom.md for the feature as a whole.
#include "Zoom.h"

#include <algorithm>
#include <atomic>
#include <string>

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "Config/ConfigManager.h"
#include "Keybinds.h"
#include "UI/Registry.h"

namespace gamescope
{
	namespace
	{
		// Config: the same lazy, generation-keyed cache Crosshair.cpp keeps,
		// plus a mirror of the fields the wlserver thread reads.
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		std::atomic<bool>  s_bEnabled{ false };
		std::atomic<bool>  s_bToggle{ false };
		std::atomic<bool>  s_bMouseScale{ false };
		std::atomic<float> s_flFactor{ 2.0f };

		// Zoomed in right now. Written on the wlserver thread (the chord),
		// read by paint_all() and by the mouse path.
		std::atomic<bool> s_bActive{ false };

		void Mirror()
		{
			const config::ZoomSettings &z = s_Settings.zoom;
			s_bEnabled.store( z.enabled, std::memory_order_relaxed );
			s_bToggle.store( z.mode == "toggle", std::memory_order_relaxed );
			s_bMouseScale.store( z.mouse_scale, std::memory_order_relaxed );
			s_flFactor.store( std::clamp( z.factor, 1.5f, 5.0f ), std::memory_order_relaxed );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			Mirror();
		}

		void PersistAndRepaint()
		{
			Mirror();
			config::EnqueueRoutedWrite( s_Settings );
			force_repaint();
		}

		// Choice rows are int-backed (Registry.h); the file keeps a word, as
		// crosshair.hide_mode does, so a config stays readable.
		constexpr ui::Option kModeOptions[] = { { 0, "Hold" }, { 1, "Toggle" } };
		constexpr ui::Option kShapeOptions[] = { { 0, "Circle" }, { 1, "Rectangle" }, { 2, "Square" } };
		constexpr const char *kShapeKeys[] = { "circle", "rectangle", "square" };

		int ShapeToInt( const std::string &s )
		{
			for ( int i = 0; i < 3; i++ )
				if ( s == kShapeKeys[ i ] ) return i;
			return 0;
		}
	}

	void Zoom_OnChord( bool bPressed )
	{
		if ( !s_bEnabled.load( std::memory_order_relaxed ) )
		{
			s_bActive.store( false, std::memory_order_relaxed );
			return;
		}
		const bool bWas = s_bActive.load( std::memory_order_relaxed );
		bool bNow = bWas;
		if ( s_bToggle.load( std::memory_order_relaxed ) )
		{
			if ( bPressed )
				bNow = !bWas;
		}
		else
			bNow = bPressed;
		if ( bNow == bWas )
			return;
		s_bActive.store( bNow, std::memory_order_relaxed );
		// The zoom coming or going is a change with no game frame attached
		// (a paused game, a menu): ask for one, as the crosshair's hook does.
		force_repaint();
	}

	float Zoom_MouseScale()
	{
		if ( !s_bActive.load( std::memory_order_relaxed )
			|| !s_bEnabled.load( std::memory_order_relaxed )
			|| !s_bMouseScale.load( std::memory_order_relaxed ) )
			return 1.0f;
		return 1.0f / s_flFactor.load( std::memory_order_relaxed );
	}

	void Zoom_FillRequest( FrameInfo_t *pFrameInfo )
	{
		EnsureConfigLoaded();
		const config::ZoomSettings &z = s_Settings.zoom;
		if ( !z.enabled || !s_bActive.load( std::memory_order_relaxed ) )
			return;

		FrameInfo_t::Zoom_t &req = pFrameInfo->zoom;
		req.bActive = true;
		req.bCircle = z.shape == "circle";
		req.flFactor = std::clamp( z.factor, 1.5f, 5.0f );
		if ( z.shape == "rectangle" )
		{
			req.flWidth = std::clamp( z.width, 0.05f, 1.0f );
			req.flHeight = std::clamp( z.height, 0.05f, 1.0f );
		}
		else
		{
			// A circle's diameter and a square's side are one number, a
			// fraction of the on-screen HEIGHT; the composite turns that
			// into pixels, so the width fraction is height * (H / W).
			const float flSize = std::clamp( z.size, 0.05f, 1.0f );
			req.flHeight = flSize;
			req.flWidth = flSize;   // corrected below from the base layer when there is one
			if ( pFrameInfo->layers.count() > 0 )
			{
				const FrameInfo_t::Layer_t &base = pFrameInfo->layers.get( 0 );
				if ( base.tex && base.scale.x > 0.0f && base.scale.y > 0.0f )
				{
					const float flW = (float)base.tex->width() / base.scale.x;
					const float flH = (float)base.tex->height() / base.scale.y;
					if ( flW > 0.0f )
						req.flWidth = std::min( 1.0f, flSize * flH / flW );
				}
			}
		}

		// The zoom samples the real base layer, so a frame with it needs
		// the base IN the composite: DRM's partial-composition shortcut and
		// the nested backends' direct scanout both take it out. Same flag,
		// same reason as the HUD's Inverted mode (rendervulkan.hpp).
		pFrameInfo->bNeedsDestinationBlend = true;
	}

	void Zoom_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.zoom", "Zoom", ui::Section::System );

		a.Keywords( "zoom magnify magnifier scope sniper aim ads hold toggle" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			return s_Settings.zoom.enabled ? std::string( "on" ) : std::string( "off" );
		} );

		auto On = []{ EnsureConfigLoaded(); return s_Settings.zoom.enabled; };
		auto RectOn = []{ EnsureConfigLoaded(); return s_Settings.zoom.enabled && s_Settings.zoom.shape == "rectangle"; };
		auto SizeOn = []{ EnsureConfigLoaded(); return s_Settings.zoom.enabled && s_Settings.zoom.shape != "rectangle"; };
		constexpr const char *kOffReason = "the zoom is off";

		using S = config::ZoomSettings;
		#define ZOOM_BIND( type, field ) \
			ui::AnyBind::Of<type>( \
				[]{ EnsureConfigLoaded(); return (type)s_Settings.zoom.field; }, \
				[]( type v ) { EnsureConfigLoaded(); s_Settings.zoom.field = v; PersistAndRepaint(); } )

		a.Group( "Zoom" );

		a.Switch( "zoom.enabled", "Enable zoom", ZOOM_BIND( bool, enabled ) )
			.Help( "Magnifies the middle of the game while the zoom key is held (or toggled). "
			       "Drawn by gamescope over the finished picture, shaders included, so it is "
			       "never blurred by the upscaler. The key itself is set under Keybinds." )
			.Default( S{}.enabled )
			.Keywords( "zoom enable show magnify" );

		a.Facts( "zoom.bind", "Zoom key",
			[]{ return keybinds::ChordTextFor( keybinds::Action::Zoom ); } )
			.Help( "The key or mouse button that zooms. Change it under Settings > Keybinds > "
			       "Zoom; mouse buttons are LMB, RMB, MMB, Mouse4 and Mouse5." )
			.Keywords( "zoom key bind keybind chord button rmb" );

		a.Choice( "zoom.mode", "Activation",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.zoom.mode == "toggle" ? 1 : 0; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.zoom.mode = n == 1 ? "toggle" : "hold"; PersistAndRepaint(); } ),
			kModeOptions, std::size( kModeOptions ) )
			.Help( "Hold: zoomed for as long as the key is down. Toggle: one press zooms in, "
			       "the next zooms out." )
			.Default( 0 )
			.Keywords( "zoom hold toggle mode activation press" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "zoom.factor", "Zoom level", ZOOM_BIND( float, factor ) )
			.Help( "How much larger the middle of the game is shown. 2 shows it at twice the size." )
			.Range( 1.5f, 5.0f ).Step( 0.1f ).Unit( "x" )
			.Default( S{}.factor )
			.Keywords( "zoom level factor magnification strength amount" )
			.DisabledUnless( On, kOffReason );

		a.Switch( "zoom.mouse_scale", "Match mouse speed", ZOOM_BIND( bool, mouse_scale ) )
			.Help( "Divides the mouse speed by the zoom level while zoomed, so the aim moves "
			       "the same distance on screen as it does unzoomed. Applied on top of "
			       "gamescope's own --mouse-sensitivity." )
			.Default( S{}.mouse_scale )
			.Keywords( "zoom mouse speed sensitivity scale aim" )
			.DisabledUnless( On, kOffReason );

		a.Group( "Projection" );

		a.Choice( "zoom.shape", "Shape",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ShapeToInt( s_Settings.zoom.shape ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.zoom.shape = kShapeKeys[ std::clamp( n, 0, 2 ) ]; PersistAndRepaint(); } ),
			kShapeOptions, std::size( kShapeOptions ) )
			.Help( "The outline the magnified picture is cut to. A circle and a square take one "
			       "size; a rectangle takes a width and a height." )
			.Default( 0 )
			.Keywords( "zoom shape circle rectangle square round" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "zoom.size", "Size", ZOOM_BIND( float, size ) )
			.Help( "The circle's diameter, or the square's side, as a share of the game's "
			       "height: 0.5 is half the screen tall, 1 is the whole height." )
			.Range( 0.05f, 1.0f ).Step( 0.05f )
			.Default( S{}.size )
			.Keywords( "zoom size diameter radius" )
			.DisabledUnless( SizeOn, "the shape is a rectangle" );

		a.Slider( "zoom.width", "Width", ZOOM_BIND( float, width ) )
			.Help( "The rectangle's width as a share of the game's width." )
			.Range( 0.05f, 1.0f ).Step( 0.05f )
			.Default( S{}.width )
			.Keywords( "zoom width rectangle" )
			.DisabledUnless( RectOn, "the shape is not a rectangle" );

		a.Slider( "zoom.height", "Height", ZOOM_BIND( float, height ) )
			.Help( "The rectangle's height as a share of the game's height." )
			.Range( 0.05f, 1.0f ).Step( 0.05f )
			.Default( S{}.height )
			.Keywords( "zoom height rectangle" )
			.DisabledUnless( RectOn, "the shape is not a rectangle" );

		#undef ZOOM_BIND
	}
}
