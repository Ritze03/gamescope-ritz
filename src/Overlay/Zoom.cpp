// The zoom -- see Zoom.h for the split between this file and the render
// side, and superdoc/features/zoom.md for the feature as a whole.
#include "Zoom.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
		std::atomic<bool>  s_bConsumeButton{ false };
		std::atomic<bool>  s_bScrollAdjust{ false };

		// Zoomed in right now. Written on the wlserver thread (the chord),
		// read by paint_all() and by the mouse path.
		std::atomic<bool> s_bActive{ false };

		// Set by Zoom_OnScroll() (wlserver thread) when it has stepped
		// s_flFactor and not yet written that back into s_Settings/config;
		// cleared and flushed by Zoom_FillRequest() (steamcompmgr thread).
		// Coalesces a fast scroll into one config write per frame.
		std::atomic<bool> s_bFactorDirty{ false };

		// The staged fade (2026-09-16, Zoom.h's kZoomFadeSplit): 0 = no
		// zoom, 1 = fully zoomed. steamcompmgr thread only -- advanced once
		// per frame in Zoom_FillRequest() from the compositor's own clock.
		// s_ulLastFadeNs is 0 whenever the fade is parked at 0 with nothing
		// wanted, so the first frame of a new press measures a zero delta
		// rather than however long the game happened to sit idle.
		float s_flFadeProgress = 0.0f;
		uint64_t s_ulLastFadeNs = 0;

		// The magnification actually on screen this frame (1.0 while not
		// zoomed). Written by Zoom_FillRequest(), read by the wlserver
		// thread's Zoom_MouseScale() so the mouse slows down WITH the ramp
		// instead of snapping to the full divisor before the picture has
		// moved.
		std::atomic<float> s_flLiveFactor{ 1.0f };

		void Mirror()
		{
			const config::ZoomSettings &z = s_Settings.zoom;
			s_bEnabled.store( z.enabled, std::memory_order_relaxed );
			s_bToggle.store( z.mode == "toggle", std::memory_order_relaxed );
			s_bMouseScale.store( z.mouse_scale, std::memory_order_relaxed );
			s_flFactor.store( std::clamp( z.factor, 1.5f, 5.0f ), std::memory_order_relaxed );
			s_bConsumeButton.store( z.consume_button, std::memory_order_relaxed );
			s_bScrollAdjust.store( z.scroll_adjust, std::memory_order_relaxed );
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
		if ( !s_bEnabled.load( std::memory_order_relaxed )
			|| !s_bMouseScale.load( std::memory_order_relaxed ) )
			return 1.0f;
		// The RAMPED magnification, not the configured one: during the
		// fade the picture is still at 1.0x, so dividing by the full factor
		// there would slow the aim before anything had been magnified.
		return 1.0f / std::max( 1.0f, s_flLiveFactor.load( std::memory_order_relaxed ) );
	}

	bool Zoom_ConsumesButton()
	{
		return s_bEnabled.load( std::memory_order_relaxed )
			&& s_bConsumeButton.load( std::memory_order_relaxed );
	}

	bool Zoom_IsActive()
	{
		return s_bActive.load( std::memory_order_relaxed );
	}

	bool Zoom_ScrollAdjustEnabled()
	{
		return s_bEnabled.load( std::memory_order_relaxed )
			&& s_bScrollAdjust.load( std::memory_order_relaxed );
	}

	void Zoom_OnScroll( double flNotches )
	{
		if ( !s_bActive.load( std::memory_order_relaxed ) )
			return;
		const int nNotches = (int)std::lround( flNotches );
		if ( nNotches == 0 )
			return;
		const float flNew = Zoom_StepFactor( s_flFactor.load( std::memory_order_relaxed ), nNotches );
		s_flFactor.store( flNew, std::memory_order_relaxed );
		s_bFactorDirty.store( true, std::memory_order_relaxed );
		force_repaint();
	}

	void Zoom_FillRequest( FrameInfo_t *pFrameInfo )
	{
		EnsureConfigLoaded();

		// Scroll-to-adjust (zoom.scroll_adjust) steps the LIVE s_flFactor
		// atomic on the wlserver thread so the picture reacts next frame;
		// config:: is single-threaded (Keybinds.h's threading note), so the
		// persisted copy is written here instead, on the steamcompmgr
		// thread that owns s_Settings, coalesced to at most one write per
		// frame no matter how many notches arrived since the last one. Runs
		// every frame (this function is called unconditionally from
		// paint_all()), not just while still zoomed, so a scroll followed
		// immediately by letting go of the zoom button still gets saved.
		if ( s_bFactorDirty.exchange( false, std::memory_order_relaxed ) )
		{
			s_Settings.zoom.factor = s_flFactor.load( std::memory_order_relaxed );
			PersistAndRepaint();
		}

		const config::ZoomSettings &z = s_Settings.zoom;

		// THE STAGED FADE (2026-09-16, Zoom.h's Zoom_AdvanceFade and
		// kZoomFadeSplit). Advanced from the compositor's own monotonic
		// clock -- the same get_time_in_nanos() every other timed thing
		// here uses -- so the reveal takes the configured wall-clock time
		// at any framerate, rather than a fixed step per frame.
		const bool bWanted = z.enabled && s_bActive.load( std::memory_order_relaxed );
		const uint64_t ulNow = get_time_in_nanos();
		const uint64_t ulDelta = ( s_ulLastFadeNs != 0 && ulNow > s_ulLastFadeNs ) ? ulNow - s_ulLastFadeNs : 0;
		s_flFadeProgress = Zoom_AdvanceFade( s_flFadeProgress, bWanted, ulDelta, z.fade_ms );
		// Parked at EITHER end, the timestamp is forgotten: force_repaint()
		// below only runs while the progress is moving, so at rest there may
		// be no next frame for however long the game sits idle, and the frame
		// that starts moving again must measure a zero delta rather than that
		// whole gap (which would snap the fade straight past its animation).
		const bool bMoving = bWanted ? s_flFadeProgress < 1.0f : s_flFadeProgress > 0.0f;
		s_ulLastFadeNs = bMoving ? ulNow : 0;

		if ( !bWanted && s_flFadeProgress <= 0.0f )
		{
			s_flLiveFactor.store( 1.0f, std::memory_order_relaxed );
			return;
		}
		// Still moving: the game may have no frame of its own coming (a
		// pause, a menu), so keep asking for one until the fade settles --
		// the crosshair's hide animation does exactly this.
		if ( bMoving )
			force_repaint();

		FrameInfo_t::Zoom_t &req = pFrameInfo->zoom;
		req.bActive = true;
		req.bCircle = z.shape == "circle";
		req.flAlpha = Zoom_FadeAlpha( s_flFadeProgress );
		// The LIVE atomic, not z.factor: a scroll-adjust step must show up
		// in the picture on the very next frame, not wait for the persist
		// flush above (which, in practice, already ran first this same
		// frame -- reading the atomic directly is what makes that true
		// regardless of ordering).
		req.flFactor = Zoom_FadeFactor( s_flFadeProgress,
			std::clamp( s_flFactor.load( std::memory_order_relaxed ), 1.5f, 5.0f ) );
		s_flLiveFactor.store( req.flFactor, std::memory_order_relaxed );
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

		a.Switch( "zoom.consume_button", "Keep the button from the game", ZOOM_BIND( bool, consume_button ) )
			.Help( "The game never receives the zoom chord's mouse button press or release. A "
			       "keyboard key is always kept from the game; a modifier such as Alt is never." )
			.Default( S{}.consume_button )
			.Keywords( "zoom consume swallow button eat hide rmb ads click" )
			.DisabledUnless( On, kOffReason );

		a.Switch( "zoom.scroll_adjust", "Scroll to change zoom level", ZOOM_BIND( bool, scroll_adjust ) )
			.Help( "While zoomed, the mouse wheel changes the zoom level in steps of 0.25 and is "
			       "not passed to the game. The new level is saved as the Zoom level above." )
			.Default( S{}.scroll_adjust )
			.Keywords( "zoom scroll wheel adjust level factor mouse" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "zoom.fade", "Fade duration", ZOOM_BIND( int, fade_ms ) )
			.Key( "zoom.fade_ms" )
			.Help( "How long the zoom takes to appear, in milliseconds. The outline fades in at "
			       "its final size first, then the picture inside it magnifies; letting go plays "
			       "the same thing backwards. 0 zooms instantly." )
			.Range( 0.0f, 2000.0f ).Step( 10.0f ).Unit( "ms" )
			.ZeroMeans( "Instant" )
			.Default( S{}.fade_ms )
			.Keywords( "zoom fade duration time animation ramp smooth speed" )
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
