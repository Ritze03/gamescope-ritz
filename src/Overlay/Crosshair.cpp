// Compositor-drawn crosshair -- see Crosshair.h for what this is and why
// it draws into the FPS HUD's layer, and superdoc/features/crosshair.md
// for the feature as a whole.
//
// Threading: everything here except Crosshair_NotifyRightButton() runs on
// the steamcompmgr thread (paint_all() -> FpsDisplay_AddLayer() ->
// Crosshair_Draw(); the Shell's setters, which draw on that same thread).
// Crosshair_NotifyRightButton() runs on the wlserver thread and only ever
// touches one atomic plus force_repaint(), which is already safe from any
// thread (the console thread calls it) -- it never reads the config cache.
#include "Crosshair.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#include "CrosshairMath.h"
#include "steamcompmgr.hpp"
#include "Config/ConfigManager.h"
#include "Config/AppId.h"
#include "UI/Registry.h"

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		// -----------------------------------------------------------------
		// Config: loaded lazily, reloaded whenever the config generation
		// moves (profile applied, override toggled) -- the same shape as
		// FpsDisplay.cpp's own cache, kept separate so neither file reaches
		// into the other's statics.
		// -----------------------------------------------------------------
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolveEffective( config::SessionAppId() );
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
		}

		// Every setter goes through here: write the file (routed to the
		// per-game snapshot when one is active, global.json otherwise) and
		// ask for a frame, so an edit is visible on the very next paint
		// even when the game itself is idle. force_repaint(), not
		// hasRepaint, for the same reason FpsDisplay.cpp gives: it is the
		// one request that reliably reaches a possibly-idle main loop.
		void PersistAndRepaint()
		{
			config::EnqueueRoutedWrite( s_Settings );
			force_repaint();
		}

		// "Right button held since <ns>", 0 while not held. Written on the
		// wlserver thread, read on the steamcompmgr thread.
		std::atomic<uint64_t> s_ulRightPressNs{ 0 };

		ImU32 PackColor( int nRgb, float flAlpha )
		{
			const int a = (int)std::lround( std::clamp( flAlpha, 0.0f, 1.0f ) * 255.0f );
			return IM_COL32( ( nRgb >> 16 ) & 0xFF, ( nRgb >> 8 ) & 0xFF, nRgb & 0xFF, a );
		}

		// Hide-mode Choice <-> stored string, same helper shape as
		// FpsDisplay.cpp's UpdateModeToInt()/UpdateModeFromInt().
		int HideModeToInt( const std::string &s )
		{
			switch ( crosshair::ParseHideMode( s ) )
			{
				case crosshair::HideMode::Focus:  return 1;
				case crosshair::HideMode::Shrink: return 2;
				default:                          return 0;
			}
		}
		const char *HideModeFromInt( int n )
		{
			switch ( n )
			{
				case 1: return crosshair::HideModeKey( crosshair::HideMode::Focus );
				case 2: return crosshair::HideModeKey( crosshair::HideMode::Shrink );
				default: return crosshair::HideModeKey( crosshair::HideMode::Fade );
			}
		}
		constexpr ui::Option kHideModeOptions[] = {
			{ 0, "Fade out" },
			{ 1, "Focus" },
			{ 2, "Shrink" },
		};
	}

	bool Crosshair_IsEnabled()
	{
		EnsureConfigLoaded();
		return s_Settings.crosshair.enabled;
	}

	void Crosshair_NotifyRightButton( bool bPressed )
	{
		if ( bPressed )
		{
			// Only the FIRST press starts the clock; a repeated press event
			// for a button already held (some backends re-report) must not
			// restart a hide that is already under way.
			uint64_t ulExpected = 0;
			s_ulRightPressNs.compare_exchange_strong( ulExpected, get_time_in_nanos() );
		}
		else
		{
			// Instant restore on release -- deliberately no reverse
			// animation. The moment the player comes off the sights they
			// want the crosshair back to re-acquire; an ease-in there would
			// be the one place a delay is actually felt.
			s_ulRightPressNs.store( 0 );
		}
		// The press/release itself is a state change with no game frame
		// attached (an idle menu, a paused game): ask for one so the
		// animation starts / the crosshair comes back without waiting for
		// the game to commit. Crosshair_Draw() keeps the frames coming
		// while the animation is moving.
		force_repaint();
	}

	bool Crosshair_Draw( ImDrawList *pDrawList, const CrosshairFrame &frame, uint64_t ulNowNs )
	{
		EnsureConfigLoaded();
		const config::CrosshairSettings &c = s_Settings.crosshair;
		if ( !c.enabled || !pDrawList )
			return false;

		crosshair::HideState hs;
		bool bAnimating = false;
		if ( c.hide_on_right_click )
		{
			const uint64_t ulPress = s_ulRightPressNs.load( std::memory_order_relaxed );
			if ( ulPress != 0 )
			{
				const float f = crosshair::HideProgress( ulPress, ulNowNs, c.hide_time_ms );
				hs = crosshair::EvaluateHide( crosshair::ParseHideMode( c.hide_mode ), f );
				bAnimating = f < 1.0f; // fully hidden is static again: no more forced frames
			}
		}

		crosshair::Style st;
		st.bLine = c.line_enabled;
		st.flLength = (float)c.line_length;
		st.flWidth = (float)c.line_width;
		st.flGap = (float)c.line_gap;
		st.bDot = c.dot_enabled;
		st.flDotSize = (float)c.dot_size;
		st.bOutline = c.outline_enabled;
		st.flOutlineWidth = (float)c.outline_width;

		crosshair::Frame fr;
		fr.flCenterX = frame.flCenterX;
		fr.flCenterY = frame.flCenterY;
		// Apply Scaling off: every size is an output pixel and the crosshair
		// is square. On: sizes are game pixels, stretched per axis exactly
		// as gamescope stretches the game -- see CrosshairFrame.
		fr.flScaleX = c.apply_scaling ? frame.flGamePixelScaleX : 1.0f;
		fr.flScaleY = c.apply_scaling ? frame.flGamePixelScaleY : 1.0f;

		const crosshair::Shape shape = crosshair::Build( st, fr, hs );
		if ( hs.flAlpha <= 0.0f || shape.Empty() )
			return bAnimating;

		// 1px mode: every primitive is an axis-aligned filled rect on whole
		// pixel coordinates (CrosshairMath.h), and anti-aliased fill is
		// switched off for exactly these draws, so a 1px line is one solid
		// pixel with no half-alpha neighbours. Restored afterwards -- the
		// FPS readout in the same draw list wants its glyphs antialiased.
		const ImDrawListFlags savedFlags = pDrawList->Flags;
		pDrawList->Flags &= ~( ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines );

		const float flOffY = frame.flDrawOffsetY;
		auto Fill = [&]( const std::vector<crosshair::IRect> &rects, ImU32 col )
		{
			if ( ( col & IM_COL32_A_MASK ) == 0 )
				return;
			for ( const crosshair::IRect &r : rects )
				pDrawList->AddRectFilled( ImVec2( (float)r.x0, (float)r.y0 + flOffY ),
				                          ImVec2( (float)r.x1, (float)r.y1 + flOffY ), col, 0.0f );
		};

		// Outline first (it is already computed as the ring OUTSIDE every
		// fill, so order only matters where the dot overlaps an arm), then
		// the arms, then the dot on top.
		if ( c.outline_enabled )
			Fill( shape.outline, PackColor( c.outline_color, c.outline_opacity * hs.flAlpha ) );
		Fill( shape.lines, PackColor( c.line_color, c.line_opacity * hs.flAlpha ) );
		Fill( shape.dot, PackColor( c.dot_color, c.dot_opacity * hs.flAlpha ) );

		pDrawList->Flags = savedFlags;
		return bAnimating;
	}

	// -------------------------------------------------------------------
	// The settings area: system.crosshair, right after the HUD's own area
	// in the rail. Groups in the user's own order -- Line, Dot, Outline,
	// Auto-hide, Scaling -- under a master switch. Every dependent row is
	// greyed with a reason while its element (or the whole crosshair) is
	// off; the master switch itself is never gated (SPEC §3.13).
	// -------------------------------------------------------------------
	void Crosshair_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.crosshair", "Crosshair", ui::Section::System );

		a.Keywords( "crosshair reticle aim dot sight overlay" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			return s_Settings.crosshair.enabled ? std::string( "on" ) : std::string( "off" );
		} );

		auto On = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled; };
		auto LineOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.line_enabled; };
		auto DotOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.dot_enabled; };
		auto OutlineOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.outline_enabled; };
		auto HideOn = []{ EnsureConfigLoaded(); return s_Settings.crosshair.enabled && s_Settings.crosshair.hide_on_right_click; };
		constexpr const char *kOffReason = "the crosshair is off";
		constexpr const char *kLineOffReason = "the line is off";
		constexpr const char *kDotOffReason = "the dot is off";
		constexpr const char *kOutlineOffReason = "the outline is off";
		constexpr const char *kHideOffReason = "auto-hide is off";

		using S = config::CrosshairSettings;
		#define CROSSHAIR_BIND( type, field ) \
			ui::AnyBind::Of<type>( \
				[]{ EnsureConfigLoaded(); return (type)s_Settings.crosshair.field; }, \
				[]( type v ) { EnsureConfigLoaded(); s_Settings.crosshair.field = v; PersistAndRepaint(); } )

		// =================================================================
		//  Crosshair
		// =================================================================
		a.Group( "Crosshair" );

		a.Switch( "crosshair.enabled", "Show crosshair", CROSSHAIR_BIND( bool, enabled ) )
			.Help( "Draws a crosshair over the middle of the game. It is drawn by gamescope, not "
			       "the game, so it stays sharp even when frame generation is smearing the "
			       "game's own crosshair, and it stays up after you close this menu." )
			.Default( S{}.enabled )
			.Keywords( "crosshair show enable reticle aim" );

		// =================================================================
		//  Line
		// =================================================================
		a.Group( "Line" );

		a.Switch( "crosshair.line", "Show lines", CROSSHAIR_BIND( bool, line_enabled ) )
			.Help( "The four arms of the crosshair. Turn them off for a dot-only crosshair." )
			.Default( S{}.line_enabled )
			.Keywords( "line arms enable show" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "crosshair.line_length", "Length", CROSSHAIR_BIND( int, line_length ) )
			.Help( "How long each arm is, in pixels. 1 is a single pixel." )
			.Range( 1.0f, 64.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.line_length )
			.Keywords( "line length size long short" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Slider( "crosshair.line_width", "Width", CROSSHAIR_BIND( int, line_width ) )
			.Help( "How thick each arm is, in pixels. 1 gives an exactly one-pixel line with no "
			       "soft edges." )
			.Range( 1.0f, 16.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.line_width )
			.Keywords( "line width thickness thin thick 1px" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Slider( "crosshair.line_gap", "Gap", CROSSHAIR_BIND( int, line_gap ) )
			.Help( "How far each arm starts from the centre, in pixels. 0 joins the arms into a "
			       "solid plus." )
			.Range( 0.0f, 64.0f ).Step( 1.0f ).Unit( "px" )
			.ZeroMeans( "None" )
			.Default( S{}.line_gap )
			.Keywords( "line gap spacing centre center distance" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Composite( "crosshair.line_color", "Colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.crosshair.line_color; },
				[]( int nPacked ) { EnsureConfigLoaded(); s_Settings.crosshair.line_color = nPacked & 0xFFFFFF; PersistAndRepaint(); } ) )
			.Help( "Colour of the arms." )
			.Default( S{}.line_color )
			.Keywords( "line colour color tint rgb" )
			.DisabledUnless( LineOn, kLineOffReason );

		a.Slider( "crosshair.line_opacity", "Opacity", CROSSHAIR_BIND( float, line_opacity ) )
			.Help( "How solid the arms are. All the way down makes them fully transparent." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( S{}.line_opacity )
			.Keywords( "line opacity transparency alpha see-through" )
			.DisabledUnless( LineOn, kLineOffReason );

		// =================================================================
		//  Dot
		// =================================================================
		a.Group( "Dot" );

		a.Switch( "crosshair.dot", "Show dot", CROSSHAIR_BIND( bool, dot_enabled ) )
			.Help( "A small square in the exact centre, on its own or inside the arms' gap." )
			.Default( S{}.dot_enabled )
			.Keywords( "dot centre center point enable show" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "crosshair.dot_size", "Size", CROSSHAIR_BIND( int, dot_size ) )
			.Help( "The dot's width and height, in pixels. 1 is a single pixel." )
			.Range( 1.0f, 16.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.dot_size )
			.Keywords( "dot size big small" )
			.DisabledUnless( DotOn, kDotOffReason );

		a.Composite( "crosshair.dot_color", "Colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.crosshair.dot_color; },
				[]( int nPacked ) { EnsureConfigLoaded(); s_Settings.crosshair.dot_color = nPacked & 0xFFFFFF; PersistAndRepaint(); } ) )
			.Help( "Colour of the dot." )
			.Default( S{}.dot_color )
			.Keywords( "dot colour color tint rgb" )
			.DisabledUnless( DotOn, kDotOffReason );

		a.Slider( "crosshair.dot_opacity", "Opacity", CROSSHAIR_BIND( float, dot_opacity ) )
			.Help( "How solid the dot is. All the way down makes it fully transparent." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( S{}.dot_opacity )
			.Keywords( "dot opacity transparency alpha" )
			.DisabledUnless( DotOn, kDotOffReason );

		// =================================================================
		//  Outline
		// =================================================================
		a.Group( "Outline" );

		a.Switch( "crosshair.outline", "Show outline", CROSSHAIR_BIND( bool, outline_enabled ) )
			.Help( "A border drawn around the arms and the dot, just outside them, so the "
			       "crosshair stays visible over bright or busy scenes." )
			.Default( S{}.outline_enabled )
			.Keywords( "outline border stroke edge enable show" )
			.DisabledUnless( On, kOffReason );

		a.Slider( "crosshair.outline_width", "Width", CROSSHAIR_BIND( int, outline_width ) )
			.Help( "How thick the outline is, in pixels." )
			.Range( 1.0f, 8.0f ).Step( 1.0f ).Unit( "px" )
			.Default( S{}.outline_width )
			.Keywords( "outline width thickness" )
			.DisabledUnless( OutlineOn, kOutlineOffReason );

		a.Slider( "crosshair.outline_opacity", "Opacity", CROSSHAIR_BIND( float, outline_opacity ) )
			.Help( "How solid the outline is. All the way down makes it fully transparent." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( S{}.outline_opacity )
			.Keywords( "outline opacity transparency alpha" )
			.DisabledUnless( OutlineOn, kOutlineOffReason );

		a.Composite( "crosshair.outline_color", "Colour", ui::CompositeKind::Color,
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.crosshair.outline_color; },
				[]( int nPacked ) { EnsureConfigLoaded(); s_Settings.crosshair.outline_color = nPacked & 0xFFFFFF; PersistAndRepaint(); } ) )
			.Help( "Colour of the outline." )
			.Default( S{}.outline_color )
			.Keywords( "outline colour color tint rgb" )
			.DisabledUnless( OutlineOn, kOutlineOffReason );

		// =================================================================
		//  Auto-hide
		// =================================================================
		a.Group( "Auto-hide" );

		a.Switch( "crosshair.hide", "Hide while holding right-click", CROSSHAIR_BIND( bool, hide_on_right_click ) )
			.Help( "Hides the crosshair while the right mouse button is held -- aiming down "
			       "sights, in most games -- and brings it back the instant you let go. Only a "
			       "right-click that reaches the game counts; clicks inside this menu never do." )
			.Default( S{}.hide_on_right_click )
			.Keywords( "hide auto autohide right click mouse button aim ads scope" )
			.DisabledUnless( On, kOffReason );

		a.Choice( "crosshair.hide_mode", "Hide mode",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return HideModeToInt( s_Settings.crosshair.hide_mode ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.crosshair.hide_mode = HideModeFromInt( n ); PersistAndRepaint(); } ),
			kHideModeOptions, std::size( kHideModeOptions ) )
			.Help( "How the crosshair goes away. Fade out simply turns transparent. Focus closes "
			       "the gap first, then fades. Shrink closes the gap first, then shrinks the arms "
			       "and the dot to nothing." )
			.Default( 0 )
			.Keywords( "hide mode fade focus shrink animation" )
			.DisabledUnless( HideOn, kHideOffReason );

		a.Slider( "crosshair.hide_time", "Time to hide", CROSSHAIR_BIND( int, hide_time_ms ) )
			.Help( "How long the hide takes from the moment you press, in milliseconds. 0 hides "
			       "at once. Coming back is always instant." )
			.Range( 0.0f, 2000.0f ).Step( 10.0f ).Unit( "ms" )
			.ZeroMeans( "Instant" )
			.Default( S{}.hide_time_ms )
			.Keywords( "hide time duration milliseconds speed" )
			.DisabledUnless( HideOn, kHideOffReason );

		// =================================================================
		//  Scaling
		// =================================================================
		a.Group( "Scaling" );

		a.Switch( "crosshair.apply_scaling", "Apply scaling", CROSSHAIR_BIND( bool, apply_scaling ) )
			.Help( "Off: sizes are screen pixels and the crosshair is always square. On: sizes "
			       "are game pixels and the crosshair is stretched exactly like the game is -- "
			       "a 4:3 game stretched to a 16:9 screen gets a wider crosshair, the way a "
			       "stretched in-game one looks." )
			.Default( S{}.apply_scaling )
			.Keywords( "scaling stretch aspect ratio 4:3 game pixels resolution" )
			.DisabledUnless( On, kOffReason );

		#undef CROSSHAIR_BIND
	}
}
