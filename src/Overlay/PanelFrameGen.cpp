// The "Frame generation" settings area -- see PanelFrameGen.h.
//
// SHAPE. Nine per-profile rows -- Frame generation (the on/off switch), Multiplier
// (2x..8x / Target fps), Target fps, Priority, Pause at refresh rate, Quality, Artifact safety, Static HUD protection,
// UI protection, Crosshair box size -- plus
// one live Status line. Every row's id IS its config key (`framegen.mode`,
// `framegen.target_fps`, ...), which is how the Shell's per-profile
// inherited/overridden dot and "Reset to inherited" find it -- no `.Key()`
// override needed (config::IsSettingsKey() answers from the serializer).
// The Multiplier row sets TWO keys (mode, and the multiplier for a fixed choice)
// but is tied to `framegen.mode`: the Shell's marker follows one key per row.
// `Why two rows:` the user asked for "a single toggle to enable and disable
// framegen in general, and then below that should just be the multiplier. It
// shouldn't be combined into one."
// Every edit calls fghost::SetConfig() at once; the renderer applies the
// presets live (a Quality change costs it one GPU wait, which is its business).
//
// `Why only these:` the library's expert knobs (searchPenalty and the
// rest) are deliberately not exposed -- only the defaults were visually
// reviewed, so a slider would hand the user settings nobody has looked at.
// `Why no keybind:` frame generation is a standing mode of the picture, not a
// momentary action; the Frame generation switch is the on/off.
#include "PanelFrameGen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Config/ConfigManager.h"
#include "FrameGen/FrameGenHost.h"
#include "main.hpp"   // g_nNestedWidth / g_nNestedHeight, the game-size fallback

namespace gamescope
{
	namespace
	{
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		// ---- string <-> enum <-> option index --------------------------------
		// The config stores readable strings (matching autoclicker.mode); the
		// Choice rows bind ints; fghost wants its own enums. These are the
		// only three places that translate, and an unknown string falls to the
		// default the schema documents.
		// The Multiplier row folds the kind of multiplier and its value into one
		// choice ("up to 8x", "a target FPS mode"): 2..8 = fixed multiplier,
		// 9 = Target fps (not a multiplier, just the next free id).
		constexpr int kTargetChoice = 9;
		constexpr ui::Option kModeOptions[] = {
			{ 2, "2×" }, { 3, "3×" }, { 4, "4×" }, { 5, "5×" },
			{ 6, "6×" }, { 7, "7×" }, { 8, "8×" }, { kTargetChoice, "Target fps" } };
		constexpr ui::Option kPriorityOptions[] = { { 0, "Low latency" }, { 1, "Smoothness" } };
		constexpr ui::Option kQualityOptions[] = { { 0, "Quality" }, { 1, "Performance" } };
		constexpr ui::Option kSafetyOptions[]  = { { 3, "Off" }, { 0, "Low" }, { 1, "Default" }, { 2, "High" } };
		constexpr ui::Option kHudOptions[]     = { { 0, "Off" }, { 1, "Normal" }, { 2, "Strong" } };
		// The ids are the fghost::UiProt values (= framegen::UiProtection's); the order
		// here is the GUI's: V2 sits next to V1 so the two can be compared at a glance.
		constexpr ui::Option kUiOptions[]      = { { 0, "Off" }, { 1, "Crosshair" }, { 3, "Crosshair V2" }, { 2, "Whole screen" } };

		constexpr const char *kPriorityKeys[] = { "low_latency", "smoothness" };
		constexpr const char *kQualityKeys[] = { "quality", "performance" };
		constexpr const char *kSafetyKeys[]  = { "low", "default", "high", "off" };
		constexpr const char *kHudKeys[]     = { "off", "normal", "strong" };
		// Indexed by fghost::UiProt, so the index IS the option id above.
		constexpr const char *kUiKeys[]      = { "off", "crosshair", "whole_screen", "crosshair_v2" };

		int IndexOf( const std::string &s, const char *const *ppszKeys, int n, int nFallback )
		{
			for ( int i = 0; i < n; ++i )
				if ( s == ppszKeys[ i ] )
					return i;
			return nFallback;
		}

		int ClampIdx( int n, int nMax ) { return n < 0 ? 0 : ( n > nMax ? nMax : n ); }

		// The row's value for the stored mode + multiplier, and back.
		int ModeChoice( const config::FrameGenSettings &f )
		{
			if ( f.mode == "target" )
				return kTargetChoice;
			if ( f.multiplier >= 2 )
				return f.multiplier > fghost::kMaxMultiplier ? fghost::kMaxMultiplier : f.multiplier;
			return 2;
		}

		void SetModeChoice( config::FrameGenSettings &f, int n )
		{
			if ( n == kTargetChoice )
			{
				f.mode = "target";
			}
			else
			{
				f.mode = "fixed";
				f.multiplier = n < 2 ? 2 : ( n > fghost::kMaxMultiplier ? fghost::kMaxMultiplier : n );
			}
		}

		// Target fps: 0 = the display's refresh, else 30..1000. A drag that lands
		// in the dead zone 1..29 snaps to whichever end is nearer, so the slider
		// can still be dragged back to "Display refresh".
		int NormalizeTarget( int n )
		{
			if ( n < 15 )
				return 0;
			return n < 30 ? 30 : ( n > 1000 ? 1000 : n );
		}

		// The Crosshair box size row is a float percent (0.5..10, step 0.1); the host
		// takes tenths of a percent.
		int BoxTenths( float flPercent )
		{
			return std::clamp( (int)std::lround( flPercent * 10.0f ), fghost::kUiBoxMinTenths, fghost::kUiBoxMaxTenths );
		}

		// The game's frame size for the "px" figure: the renderer's own once it has
		// seen a frame (the layer-0 size is what the library protects), else the
		// nested resolution the game was asked for.
		void GameSize( uint32_t *puW, uint32_t *puH )
		{
			if ( !fghost::GameFrameSize( puW, puH ) )
			{
				*puW = g_nNestedWidth > 0 ? (uint32_t)g_nNestedWidth : 1920u;
				*puH = g_nNestedHeight > 0 ? (uint32_t)g_nNestedHeight : 1080u;
			}
		}

		// "2.5% . 24 px": the setting and what it comes to for this game. Pure over
		// its inputs (the live caller below feeds the game size), so the wording is
		// one place.
		std::string BoxSizeText( float flPercent, uint32_t uGameW, uint32_t uGameH )
		{
			uint32_t uBoxW = 0, uBoxH = 0;
			fghost::UiBoxPixels( uGameW, uGameH, BoxTenths( flPercent ), &uBoxW, &uBoxH );
			char sz[ 64 ];
			std::snprintf( sz, sizeof( sz ), "%.1f%% · %u px", flPercent, uBoxW );
			return sz;
		}

		fghost::Config ToHostConfig( const config::FrameGenSettings &f )
		{
			fghost::Config c;
			c.enabled = f.enabled;
			c.mode = f.mode == "target" ? fghost::Mode::Target : fghost::Mode::Fixed;
			c.multiplier = f.multiplier;   // SetConfig() normalises (fixed below 2x -> disabled, above 8x -> 8x)
			c.targetFps = f.target_fps;
			c.priority = IndexOf( f.priority, kPriorityKeys, 2, 1 ) == 1
				? fghost::Priority::Smoothness : fghost::Priority::LowLatency;
			c.pauseAtRefresh = f.pause_at_refresh;
			c.gpuLimit = f.gpu_limit;
			c.quality = IndexOf( f.quality, kQualityKeys, 2, 0 ) == 1
				? fghost::Quality::Performance : fghost::Quality::Quality;
			c.safety = (fghost::Safety)IndexOf( f.safety, kSafetyKeys, 4, 3 );
			c.hud = (fghost::HudProtect)IndexOf( f.hud_protection, kHudKeys, 3, 2 );
			c.ui = (fghost::UiProt)IndexOf( f.ui_protection, kUiKeys, 4, 1 );
			c.uiBoxTenths = BoxTenths( f.ui_box_height );
			return c;
		}

		// The one place config state becomes renderer state: after every edit,
		// after every (re)load, and at startup / profile switch via
		// PanelFrameGen_ApplyStartupConfig().
		void PushToRenderer( const config::FrameGenSettings &f )
		{
			fghost::SetConfig( ToHostConfig( f ) );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			// A (re)load -- first draw, or a profile switch -- applies now,
			// not when the user next touches a row.
			PushToRenderer( s_Settings.framegen );
		}

		void PersistAndPush()
		{
			PushToRenderer( s_Settings.framegen );
			config::EnqueueRoutedWrite( s_Settings );
		}

		bool On()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled;
		}

		bool UiOn()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled && s_Settings.framegen.ui_protection != "off";
		}

		// Pause at refresh rate: a Target already caps the output at the target
		// (0 = the refresh), so the switch only governs the fixed multipliers.
		bool PauseRowUsable()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled && s_Settings.framegen.mode != "target";
		}

		bool TargetMode()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.mode == "target";
		}

		// ---- the Status line (decision D18) -----------------------------------
		// One compact line, always. Pure over its inputs so the strings are one
		// place; the live caller below feeds it the two fghost snapshots.
		//   Off                       -> "Off"
		//   generating (activeN >= 2) -> target: "60->280 · target 280 (×4.6) · +13 ms"
		//                                fixed:  "60->120 · 2× · +8 ms"
		//                                (a fixed multiplier the display saturates reads
		//                                 "60->280 · 8× (×4.6) · +13 ms"; a 10-bit / HDR
		//                                 game appends " · 10-bit" / " · HDR")
		//   otherwise the reason      -> the renderer's (HDR, 10-bit, YCbCr, ...) when it
		//                                 refuses, else pacing's (game already at the
		//                                 refresh or target, GPU too slow, VRR, warming
		//                                 up, ...)
		//   nothing published yet     -> "Waiting for frames"
		// The font is Basic Latin + Latin-1 only, so the arrow is "->" (U+2192
		// is not in the atlas); "·" and "×" are Latin-1 and render. The FG / UI GPU
		// times used to be on this line; the user found it overloaded and they now
		// live only in the debug log.
		std::string StatusLine( const config::FrameGenSettings &f, const fghost::PacingStatus &ps, const fghost::RenderStatus &rs )
		{
			if ( !f.enabled )
				return "Off";
			const int nChoice = ModeChoice( f );

			if ( ps.valid && ps.activeN >= 2 )
			{
				char sz[ 192 ];
				int n = std::snprintf( sz, sizeof( sz ), "%.0f->%.0f", ps.gameFps, ps.presentedFps );
				std::string s( sz, n > 0 ? (size_t)n : 0 );
				if ( nChoice == kTargetChoice )
				{
					std::snprintf( sz, sizeof( sz ), " · target %.0f (×%.1f)", ps.targetFps, ps.effectiveMultiplier );
					s += sz;
				}
				else
				{
					std::snprintf( sz, sizeof( sz ), " · %d×", nChoice );
					s += sz;
					// The display cannot show N x the game: say what it actually does.
					if ( ps.effectiveMultiplier > 0.0f && ps.effectiveMultiplier < float( nChoice ) - 0.15f )
					{
						std::snprintf( sz, sizeof( sz ), " (×%.1f)", ps.effectiveMultiplier );
						s += sz;
					}
				}
				std::snprintf( sz, sizeof( sz ), " · +%.0f ms", ps.delayMs );
				s += sz;
				// The game's format when it is not plain 8-bit: "HDR", "10-bit", "16-bit float".
				if ( rs.formatTag && rs.formatTag[ 0 ] )
				{
					s += " · ";
					s += rs.formatTag;
				}
				return s;
			}

			if ( rs.reason != fghost::Unavailable::Ok )
				return fghost::UnavailableText( rs.reason );

			if ( ps.valid )
			{
				const std::string sReason = fghost::PassReasonText( ps.reason );
				if ( !sReason.empty() )
					return sReason;
			}
			return "Waiting for frames";
		}
	}

	void PanelFrameGen_ApplyStartupConfig( const config::Settings &config )
	{
		PushToRenderer( config.framegen );
	}

	void PanelFrameGen_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "image.framegen", "Frame generation", ui::Section::Display );

		a.Keywords( "frame generation framegen interpolation fluid motion smooth fps multiplier "
		            "target priority latency smoothness optical flow lsfg fsr3 fmf frames" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			if ( !s_Settings.framegen.enabled )
				return std::string( "off" );
			const int n = ModeChoice( s_Settings.framegen );
			if ( n == kTargetChoice )
				return s_Settings.framegen.target_fps > 0
					? "target " + std::to_string( s_Settings.framegen.target_fps ) : std::string( "target" );
			return std::to_string( n ) + "×";
		} );

		using S = config::FrameGenSettings;

		a.Group( "Frame generation" );

		a.Switch( "framegen.enabled", "Frame generation",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.enabled; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.enabled = b; PersistAndPush(); } ) )
			.Help( "Generates extra frames between the game's own frames. Adds delay, so it is "
			       "not suited to twitch shooters." )
			.Default( false )
			.Keywords( "frame generation framegen fg interpolation fluid motion smooth fps enable on off switch toggle" );

		a.Choice( "framegen.mode", "Multiplier",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ModeChoice( s_Settings.framegen ); },
				[]( int n ) { EnsureConfigLoaded(); SetModeChoice( s_Settings.framegen, n ); PersistAndPush(); } ),
			kModeOptions, std::size( kModeOptions ) )
			.Help( "How many frames are shown per game frame, 2× to 8× the game's rate, or aim at "
			       "a Target fps. It keeps generating until the game reaches your refresh rate (or "
			       "the target); a multiplier your display cannot show just fills every refresh. "
			       "The Frame limiter caps the game and this multiplies the capped rate." )
			.Default( kTargetChoice )
			.Keywords( "frame generation framegen fg multiplier 2x 3x 4x 5x 6x 7x 8x double triple quadruple target fps dynamic interpolation" )
			.DisabledUnless( On, "frame generation is off" );

		a.Slider( "framegen.target_fps", "Target fps",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.target_fps; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.target_fps = NormalizeTarget( n ); PersistAndPush(); } ) )
			.Help( "The frame rate to aim for in Target fps mode. The number of generated frames "
			       "follows the game's frame times on every frame: if the game drops from 120 to "
			       "100 fps with a target of 240, it generates more per game frame at once. 0 "
			       "aims at your display's refresh rate; a target above it is capped to it." )
			.Range( 0.0f, 1000.0f ).Step( 5.0f ).Unit( "fps" )
			.ZeroMeans( "Display refresh" )
			.Default( 0 )
			.Keywords( "frame generation target fps frame rate dynamic refresh" )
			.DisabledUnless( TargetMode, "Multiplier is not set to Target fps" );

		a.Choice( "framegen.priority", "Priority",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.priority, kPriorityKeys, 2, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.priority = kPriorityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kPriorityOptions, std::size( kPriorityOptions ) )
			.Help( "Low latency adds the least delay, but motion can stutter when the game's "
			       "frame times jitter. Smoothness spaces the frames perfectly evenly and adds "
			       "about one game frame of delay." )
			.Default( 1 )
			.Keywords( "frame generation priority low latency smoothness input lag jitter pacing" )
			.DisabledUnless( On, "frame generation is off" );

		a.Switch( "framegen.pause_at_refresh", "Pause at refresh rate",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.pause_at_refresh; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.pause_at_refresh = b; PersistAndPush(); } ) )
			.Help( "Only for the fixed multipliers (2× to 8×); with Target fps the target already caps "
			       "the output, so this has no effect there. On: frame generation stops once "
			       "the game alone reaches your refresh rate, so the GPU does no pointless work. "
			       "Off: it keeps generating above your refresh rate (e.g. 800 fps): useful with "
			       "tearing or in a desktop window, where only the newest frame is shown; it "
			       "costs GPU time, and on a real display without tearing it cannot go past "
			       "the refresh rate." )
			.Default( true )
			.Keywords( "frame generation pause refresh rate stop cap limit above uncapped tearing multiplier" )
			.DisabledUnless( PauseRowUsable, "frame generation is off, or Multiplier is Target fps (the target already caps the output)" );

		a.Switch( "framegen.gpu_limit", "Limit to GPU speed",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.gpu_limit; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.gpu_limit = b; PersistAndPush(); } ) )
			.Help( "On: when frame generation takes too much of the GPU it lowers the output rate, "
			       "and as a last resort shows only real frames, so the game itself keeps its frame "
			       "rate. Off: always generate at the chosen rate, even if that slows the game down." )
			.Default( false )
			.Keywords( "frame generation limit gpu speed cost guard keep up slow lower output rate pass through" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.quality", "Quality",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.quality, kQualityKeys, 2, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.quality = kQualityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kQualityOptions, std::size( kQualityOptions ) )
			.Help( "Performance is roughly 35-50% cheaper on the GPU (1440p 2x: 0.50 -> 0.25 ms per "
			       "game frame; 4x: 0.71 -> 0.46 ms, RX 7900 XTX) but estimates motion at a quarter "
			       "resolution and can miss thin, fast detail. Quality is the default look. "
			       "Switching pauses the picture for an instant." )
			.Default( 0 )
			.Keywords( "frame generation quality performance cost gpu motion estimation" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.safety", "Artifact safety",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.safety, kSafetyKeys, 4, 3 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.safety = kSafetyKeys[ ClampIdx( n, 3 ) ]; PersistAndPush(); } ),
			kSafetyOptions, std::size( kSafetyOptions ) )
			.Help( "Decides when a pixel the motion estimate got wrong falls back to the real frame. "
			       "Each level sets the mismatch range (0-255 levels) between fully interpolated and "
			       "fully real: High 8-28, Default 12-40, Low 16-56. All three also show the real "
			       "frame when more than 15% of the picture is untrusted or on a scene cut. Off "
			       "never falls back: smoothest, but expect smearing on fast flicks and blended "
			       "scene cuts." )
			.Default( 3 )
			.Keywords( "frame generation artifact safety artefacts ghosting trust fallback" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.hud_protection", "Static HUD protection",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.hud_protection, kHudKeys, 3, 2 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.hud_protection = kHudKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kHudOptions, std::size( kHudOptions ) )
			.Help( "A soft bias towards \"stay put\" when frames are generated, so a motionless "
			       "or see-through HUD wobbles less. It only nudges the motion estimate: it "
			       "cannot make a HUD pixel-exact (that is UI protection below). Strong "
			       "can hold back genuinely slow motion near the HUD." )
			.Default( 2 )
			.Keywords( "frame generation static hud protection ui wobble see-through" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.ui_protection", "UI protection",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.ui_protection, kUiKeys, 4, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.ui_protection = kUiKeys[ ClampIdx( n, 3 ) ]; PersistAndPush(); } ),
			kUiOptions, std::size( kUiOptions ) )
			.Help( "Keeps a still crosshair in the middle of the screen pixel-exact. Crosshair V2 "
			       "only protects pixels that stay still and stand out from what is behind them, "
			       "never pastes over moving content and has no fill-in step, so the specks and "
			       "moving patterns around the crosshair are gone (an opaque, unchanging crosshair "
			       "only). Crosshair is the older method, kept to compare. Whole screen does the "
			       "older method for every solid still HUD element too (minimap, ammo, text), at "
			       "a small extra cost. None can protect see-through or changing UI. Off for "
			       "racing games or anything without a fixed HUD. Select this row or the box size "
			       "to see what lies under the box." )
			.Default( 1 )
			.Preview( ui::Entry::PreviewKind::UiBox )
			.Keywords( "frame generation ui protection crosshair v2 hud whole screen minimap static pixel exact specks artifacts" )
			.DisabledUnless( On, "frame generation is off" );

		a.Slider( "framegen.ui_box_height", "Crosshair box size",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.ui_box_height; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.ui_box_height = std::clamp( std::round( fl * 10.0f ) / 10.0f, 0.5f, 10.0f );
					PersistAndPush();
				} ) )
			.Help( "The side of the protected square in the middle of the screen, as a share of the "
			       "game's height. Small is better: everything inside it is pasted from the real "
			       "frame, so a box much bigger than the crosshair also holds some of the "
			       "background. Make it just big enough for the whole crosshair - the picture "
			       "in the Inspector shows exactly what it covers." )
			.Range( 0.5f, 10.0f ).Step( 0.1f )
			.Default( 2.5f )
			.ValueText( []( const ui::Value &v )
			{
				uint32_t uW = 0, uH = 0;
				GameSize( &uW, &uH );
				return BoxSizeText( std::holds_alternative<float>( v ) ? std::get<float>( v ) : 2.5f, uW, uH );
			} )
			.Preview( ui::Entry::PreviewKind::UiBox )
			.Keywords( "frame generation ui protection crosshair box size area percent height pixels specks" )
			.DisabledUnless( UiOn, "UI protection or frame generation is off" );

		a.Group( "Status" );

		a.Facts( "framegen.status", "Status", []
		{
			EnsureConfigLoaded();
			return StatusLine( s_Settings.framegen, fghost::GetPacingStatus(), fghost::GetRenderStatus() );
		} )
			.Help( "What frame generation is doing right now, or why it is passing real frames "
			       "through: the game already reaching the refresh rate or target, a GPU too slow "
			       "to keep up, variable refresh, or HDR / 10-bit content. When the GPU is the "
			       "limit it generates fewer frames before giving up. MangoHud with output timing "
			       "shows the presented rate; this fork's HUD shows the game rate, the "
			       "presented rate, or both, per its FPS shown choice (Game / Output / Both)." )
			.Keywords( "frame generation status fps presented delay latency reason" );
	}
}
