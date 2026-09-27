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
#include "PanelKeybinds.h"

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
		std::atomic<bool>  s_bMouseScaleHiddenOnly{ false };
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

		// The projector's shape and placement, in the game SURFACE's own
		// coordinate space -- mirrored every frame by Zoom_FillRequest()
		// (steamcompmgr thread) for Zoom_MapPointerForGame() (wlserver
		// thread) to read. See Zoom.h's ZoomRemapGeometry for what each
		// field means and superdoc/features/zoom.md's "Clicks land on what
		// the projector shows" for why surface space and not output pixels.
		// s_bGeomValid is false whenever no projector is actually being
		// drawn this frame (zoom off, no base layer yet, or a base the
		// zoom itself would skip -- HDR, YCbCr, PASSTHRU -- mirroring
		// rendervulkan.cpp's own THE ZOOM gate so the remap never acts on
		// geometry nothing is drawn to justify).
		std::atomic<bool>  s_bGeomValid{ false };
		std::atomic<bool>  s_bGeomCircle{ false };
		std::atomic<double> s_flGeomCenterX{ 0.0 };
		std::atomic<double> s_flGeomCenterY{ 0.0 };
		std::atomic<double> s_flGeomHalfW{ 0.0 };
		std::atomic<double> s_flGeomHalfH{ 0.0 };

		void Mirror()
		{
			const config::ZoomSettings &z = s_Settings.zoom;
			s_bEnabled.store( z.enabled, std::memory_order_relaxed );
			s_bToggle.store( z.mode == "toggle", std::memory_order_relaxed );
			s_bMouseScale.store( z.mouse_scale, std::memory_order_relaxed );
			s_bMouseScaleHiddenOnly.store( z.mouse_scale_hidden_only, std::memory_order_relaxed );
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

	float Zoom_MouseScale( bool bCursorHidden )
	{
		if ( !s_bEnabled.load( std::memory_order_relaxed )
			|| !s_bMouseScale.load( std::memory_order_relaxed ) )
			return 1.0f;
		// "Only while the cursor is hidden" (2026-09-25): opt-in, off by
		// default -- the user's own reason: "it will probably be buggy in
		// some games" (a game whose hidden-cursor signalling this fork
		// misreads would otherwise slow the mouse somewhere the user did
		// not ask for it). See Zoom.h's Zoom_MouseScale() for what
		// `bCursorHidden` means.
		if ( s_bMouseScaleHiddenOnly.load( std::memory_order_relaxed ) && !bCursorHidden )
			return 1.0f;
		// The RAMPED magnification, not the configured one: during the
		// fade the picture is still at 1.0x, so dividing by the full factor
		// there would slow the aim before anything had been magnified.
		return 1.0f / std::max( 1.0f, s_flLiveFactor.load( std::memory_order_relaxed ) );
	}

	void Zoom_MapPointerForGame( double &x, double &y, bool bCursorVisible )
	{
		// Cheap early-out for the overwhelmingly common case (zoom off, or
		// on but nothing drawn this frame): one relaxed atomic load, no
		// further reads. Mirrors the same short-circuit shape
		// Zoom_MouseScale() already uses.
		if ( !bCursorVisible || !s_bGeomValid.load( std::memory_order_relaxed ) )
			return;
		ZoomRemapGeometry geo;
		geo.bVisible = true;
		geo.shape = s_bGeomCircle.load( std::memory_order_relaxed ) ? ZoomShape::Circle : ZoomShape::Box;
		geo.flCenterX = s_flGeomCenterX.load( std::memory_order_relaxed );
		geo.flCenterY = s_flGeomCenterY.load( std::memory_order_relaxed );
		geo.flHalfW = s_flGeomHalfW.load( std::memory_order_relaxed );
		geo.flHalfH = s_flGeomHalfH.load( std::memory_order_relaxed );
		geo.flFactor = (double)s_flLiveFactor.load( std::memory_order_relaxed );
		ZoomRemapPoint( x, y, bCursorVisible, geo );
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
			s_bGeomValid.store( false, std::memory_order_relaxed );
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
		const float flTargetFactor = std::clamp( s_flFactor.load( std::memory_order_relaxed ), 1.5f, 5.0f );
		req.flFactor = Zoom_FadeFactor( s_flFadeProgress, flTargetFactor );
		s_flLiveFactor.store( req.flFactor, std::memory_order_relaxed );
		// Sharpen (2026-09-22, retuned 2026-09-22): the 0..1 slider maps to
		// a shader-units amount through Zoom_SharpenAmount() (Zoom.h, next
		// to this ramp -- see its own comment for the measured curve and
		// why it is not linear), and THAT amount is what the fade ramp
		// scales -- 0 for as long as req.flFactor is still pinned at 1.0 by
		// the fade's phase 1, ramping to the full mapped amount in step
		// with the magnification itself, exactly as before this retune.
		// req.flSharpen carries the already-mapped, already-ramped AMOUNT
		// from here on, not a 0..1 fraction.
		req.flSharpen = Zoom_SharpenAmount( std::clamp( z.sharpen, 0.0f, 1.0f ) )
			* Zoom_SharpenRamp( req.flFactor, flTargetFactor );
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

		// Mirror the projector's geometry in SURFACE space for
		// Zoom_MapPointerForGame() (the wlserver thread), so a click lands
		// on what the projector shows -- superdoc/features/zoom.md's
		// "Clicks land on what the projector shows". Gated the same way
		// rendervulkan.cpp's THE ZOOM block gates actually drawing one
		// (SDR, non-YCbCr base; g_uBaseLayerSourceWidth/Height known) --
		// duplicated here rather than read back from there because that
		// block runs later, in vulkan_composite(), by which point this
		// function has already returned; keep the two gates in sync if
		// either one changes. req.flWidth/flHeight are already the exact
		// on-screen-rect FRACTION rendervulkan.cpp turns into pixels, so
		// applying that same fraction to the game's own SURFACE size
		// (g_uBaseLayerSourceWidth/Height) gives the shape's surface-space
		// half-extents directly -- no separate "output pixels per surface
		// pixel" factor needed, and no risk of it disagreeing with the
		// pixel geometry's own rounding/clamping (that clamp only ever
		// bites at extreme sizes rendervulkan.cpp also clamps to
		// [4, output], irrelevant at the surface-space precision a click
		// needs).
		bool bGeomValid = false;
		if ( pFrameInfo->layers.count() > 0 && g_uBaseLayerSourceWidth > 0 && g_uBaseLayerSourceHeight > 0 )
		{
			const FrameInfo_t::Layer_t &base = pFrameInfo->layers.get( 0 );
			const bool bSdr = base.tex
				&& !ColorspaceIsHDR( base.colorspace )
				&& base.colorspace != GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU
				&& !base.isYcbcr();
			if ( bSdr )
			{
				s_flGeomCenterX.store( (double)g_uBaseLayerSourceWidth * 0.5, std::memory_order_relaxed );
				s_flGeomCenterY.store( (double)g_uBaseLayerSourceHeight * 0.5, std::memory_order_relaxed );
				s_flGeomHalfW.store( (double)req.flWidth * (double)g_uBaseLayerSourceWidth * 0.5, std::memory_order_relaxed );
				s_flGeomHalfH.store( (double)req.flHeight * (double)g_uBaseLayerSourceHeight * 0.5, std::memory_order_relaxed );
				s_bGeomCircle.store( req.bCircle, std::memory_order_relaxed );
				bGeomValid = true;
			}
		}
		s_bGeomValid.store( bGeomValid, std::memory_order_relaxed );
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
			       "never blurred by the upscaler. The key is set just below." )
			.Default( S{}.enabled )
			.Keywords( "zoom enable show magnify" );

		// The zoom's own hotkey row, editable here since 2026-09-22 (the user:
		// "Move the auto-clicker hotkey and the zoom hotkey into their
		// respective tabs"). The same chord as Keybinds' `keybinds.zoom` --
		// PanelKeybinds_ChordRow() gives both the one binding -- so it is
		// shared by every profile even though the rest of this area is not.
		PanelKeybinds_ChordRow( a, "zoom.bind", "Zoom key", keybinds::Action::Zoom )
			.Help( "The key or mouse button that zooms. Click it, then press the new key; mouse "
			       "buttons are LMB, RMB, MMB, Mouse4 and Mouse5. Shared by every profile, and "
			       "also listed under Keybinds." )
			.Keywords( "zoom key bind keybind hotkey chord button rmb rebind" );

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
			       "gamescope's own --mouse-sensitivity. Never applied to the settings overlay's "
			       "own mouse, even while it is open over a zoomed game." )
			.Default( S{}.mouse_scale )
			.Keywords( "zoom mouse speed sensitivity scale aim" )
			.DisabledUnless( On, kOffReason );

		auto MouseScaleOn = []{ EnsureConfigLoaded(); return s_Settings.zoom.enabled && s_Settings.zoom.mouse_scale; };
		constexpr const char *kMouseScaleOffReason = "\"Match mouse speed\" is off";

		a.Switch( "zoom.mouse_scale_hidden_only", "Only while the cursor is hidden", ZOOM_BIND( bool, mouse_scale_hidden_only ) )
			.Help( "Leaves the mouse speed alone whenever the game shows a cursor (menus, "
			       "inventories) and only divides it while the game's own cursor is hidden. Off "
			       "by default: this may misjudge some games, so it is opt-in rather than always "
			       "on." )
			.Default( S{}.mouse_scale_hidden_only )
			.Keywords( "zoom mouse speed hidden cursor visible menu only" )
			.DisabledUnless( MouseScaleOn, kMouseScaleOffReason );

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
			       "its final size over the first 10% of that time, then the picture inside it "
			       "magnifies over the other 90%; letting go plays the same thing backwards. 0 "
			       "zooms instantly." )
			.Range( 0.0f, 2000.0f ).Step( 10.0f ).Unit( "ms" )
			.ZeroMeans( "Instant" )
			.Default( S{}.fade_ms )
			.Keywords( "zoom fade duration time animation ramp smooth speed" )
			.DisabledUnless( On, kOffReason );

		// Sharpen (2026-09-22, retuned 2026-09-22). The user: "add an option
		// to add a sharpening filter on top of the projector area only, so
		// the zoom content doesn't look as blurry as it does right now." A
		// contrast-clamped unsharp mask inside cs_zoom.comp -- see that file
		// and ZoomPushData_t (rendervulkan.cpp) for the amount mapping, and
		// Zoom.h's Zoom_SharpenRamp for why it never applies during the
		// fade's pinned-at-1.0x first phase. 0 (default) is off and
		// byte-identical to before this row existed.
		a.Slider( "zoom.sharpen", "Sharpen", ZOOM_BIND( float, sharpen ) )
			.Help( "Sharpens the magnified picture inside the projector, so it looks less blurry. "
			       "Only the projector is affected, never the rest of the screen. 0 is off." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.ZeroMeans( "Off" )
			.Default( S{}.sharpen )
			.Keywords( "zoom sharpen sharpness crisp clarity unsharp blur blurry" )
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
