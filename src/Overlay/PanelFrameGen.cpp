// The "Frame generation" settings area -- see PanelFrameGen.h.
//
// SHAPE. Six per-profile rows -- Frame generation (Off / 2x..8x / Target fps),
// Target fps, Priority, Quality, Artifact safety, Static HUD protection -- plus
// one live Status line. Every row's id IS its config key (`framegen.mode`,
// `framegen.target_fps`, ...), which is how the Shell's per-profile
// inherited/overridden dot and "Reset to inherited" find it -- no `.Key()`
// override needed (config::IsSettingsKey() answers from the serializer).
// The first row sets TWO keys (mode, and the multiplier for a fixed choice) but
// is tied to `framegen.mode`: the Shell's marker follows one key per row.
// Every edit calls fghost::SetConfig() at once; the renderer applies the
// presets live (a Quality change costs it one GPU wait, which is its business).
//
// `Why only these:` the library's expert knobs (searchPenalty and the
// rest) are deliberately not exposed -- only the defaults were visually
// reviewed, so a slider would hand the user settings nobody has looked at.
// `Why no keybind:` frame generation is a standing mode of the picture, not a
// momentary action; the Frame generation row is the switch.
#include "PanelFrameGen.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "Config/ConfigManager.h"
#include "FrameGen/FrameGenHost.h"

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
		// The first row folds the mode and the multiplier into one choice, as the
		// user asked ("up to 8x", "a target FPS mode"): 0 = Off, 2..8 = fixed
		// multiplier, 9 = Target fps (not a multiplier, just the next free id).
		constexpr int kTargetChoice = 9;
		constexpr ui::Option kModeOptions[] = {
			{ 0, "Off" }, { 2, "2×" }, { 3, "3×" }, { 4, "4×" }, { 5, "5×" },
			{ 6, "6×" }, { 7, "7×" }, { 8, "8×" }, { kTargetChoice, "Target fps" } };
		constexpr ui::Option kPriorityOptions[] = { { 0, "Low latency" }, { 1, "Smoothness" } };
		constexpr ui::Option kQualityOptions[] = { { 0, "Quality" }, { 1, "Performance" } };
		constexpr ui::Option kSafetyOptions[]  = { { 0, "Low" }, { 1, "Default" }, { 2, "High" } };
		constexpr ui::Option kHudOptions[]     = { { 0, "Off" }, { 1, "Normal" }, { 2, "Strong" } };

		constexpr const char *kPriorityKeys[] = { "low_latency", "smoothness" };
		constexpr const char *kQualityKeys[] = { "quality", "performance" };
		constexpr const char *kSafetyKeys[]  = { "low", "default", "high" };
		constexpr const char *kHudKeys[]     = { "off", "normal", "strong" };

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
			if ( f.mode == "fixed" && f.multiplier >= 2 )
				return f.multiplier > fghost::kMaxMultiplier ? fghost::kMaxMultiplier : f.multiplier;
			return 0;
		}

		void SetModeChoice( config::FrameGenSettings &f, int n )
		{
			if ( n == kTargetChoice )
			{
				f.mode = "target";
			}
			else if ( n >= 2 )
			{
				f.mode = "fixed";
				f.multiplier = n > fghost::kMaxMultiplier ? fghost::kMaxMultiplier : n;
			}
			else
			{
				f.mode = "off";
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

		fghost::Config ToHostConfig( const config::FrameGenSettings &f )
		{
			fghost::Config c;
			c.mode = f.mode == "target" ? fghost::Mode::Target
				: ( f.mode == "fixed" ? fghost::Mode::Fixed : fghost::Mode::Off );
			c.multiplier = f.multiplier;   // SetConfig() normalises (fixed below 2x -> Off, above 8x -> 8x)
			c.targetFps = f.target_fps;
			c.priority = IndexOf( f.priority, kPriorityKeys, 2, 0 ) == 1
				? fghost::Priority::Smoothness : fghost::Priority::LowLatency;
			c.quality = IndexOf( f.quality, kQualityKeys, 2, 0 ) == 1
				? fghost::Quality::Performance : fghost::Quality::Quality;
			c.safety = (fghost::Safety)IndexOf( f.safety, kSafetyKeys, 3, 1 );
			c.hud = (fghost::HudProtect)IndexOf( f.hud_protection, kHudKeys, 3, 1 );
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
			return ModeChoice( s_Settings.framegen ) != 0;
		}

		bool TargetMode()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.mode == "target";
		}

		// ---- the Status line (decision D18) -----------------------------------
		// One line, always. Pure over its inputs so the strings are one place;
		// the live caller below feeds it the two fghost snapshots.
		//   Off                       -> "Off"
		//   generating (activeN >= 2) -> fixed:  "game 60 fps -> presented ~120 fps · 2×
		//                                          · FG 0.42 ms · +8.3 ms delay"
		//                                (a fixed multiplier the display saturates reads
		//                                 "2× (1.4× actual)")
		//                                target: "game 100 fps -> presented ~240 fps ·
		//                                          target 240 (2.4×) · FG 0.50 ms · +6.3 ms delay"
		//   otherwise the reason      -> the renderer's (HDR, 10-bit, YCbCr, ...) when it
		//                                 refuses, else pacing's (game already at the
		//                                 refresh or target, GPU too slow, VRR, warming
		//                                 up, ...)
		//   nothing published yet     -> "Waiting for frames"
		// The font is Basic Latin + Latin-1 only, so the arrow is "->" (U+2192
		// is not in the atlas); "·" and "×" are Latin-1 and render.
		std::string StatusLine( const config::FrameGenSettings &f, const fghost::PacingStatus &ps, const fghost::RenderStatus &rs )
		{
			const int nChoice = ModeChoice( f );
			if ( nChoice == 0 )
				return "Off";

			if ( ps.valid && ps.activeN >= 2 )
			{
				char sz[ 192 ];
				int n = std::snprintf( sz, sizeof( sz ), "game %.0f fps -> presented ~%.0f fps",
					ps.gameFps, ps.presentedFps );
				std::string s( sz, n > 0 ? (size_t)n : 0 );
				if ( nChoice == kTargetChoice )
				{
					std::snprintf( sz, sizeof( sz ), " · target %.0f (%.1f×)", ps.targetFps, ps.effectiveMultiplier );
					s += sz;
				}
				else
				{
					std::snprintf( sz, sizeof( sz ), " · %d×", nChoice );
					s += sz;
					// The display cannot show N x the game: say what it actually does.
					if ( ps.effectiveMultiplier > 0.0f && ps.effectiveMultiplier < float( nChoice ) - 0.15f )
					{
						std::snprintf( sz, sizeof( sz ), " (%.1f× actual)", ps.effectiveMultiplier );
						s += sz;
					}
				}
				if ( rs.lastPairGpuMs < 0.0f )
					s += " · FG n/a";
				else
				{
					std::snprintf( sz, sizeof( sz ), " · FG %.2f ms", rs.lastPairGpuMs );
					s += sz;
				}
				std::snprintf( sz, sizeof( sz ), " · +%.1f ms delay", ps.delayMs );
				s += sz;
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
			const int n = ModeChoice( s_Settings.framegen );
			if ( n == kTargetChoice )
				return s_Settings.framegen.target_fps > 0
					? "target " + std::to_string( s_Settings.framegen.target_fps ) : std::string( "target" );
			return n >= 2 ? std::to_string( n ) + "×" : std::string( "off" );
		} );

		using S = config::FrameGenSettings;

		a.Group( "Frame generation" );

		a.Choice( "framegen.mode", "Frame generation",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ModeChoice( s_Settings.framegen ); },
				[]( int n ) { EnsureConfigLoaded(); SetModeChoice( s_Settings.framegen, n ); PersistAndPush(); } ),
			kModeOptions, std::size( kModeOptions ) )
			.Help( "Shows extra frames between the game's own by generating them, 2× to 8× the "
			       "game's rate, or aiming at a Target fps. It keeps generating until the game "
			       "reaches your refresh rate (or the target); a multiplier your display cannot "
			       "show just fills every refresh. Adds delay, so it is not suited to twitch "
			       "shooters. The Frame limiter caps the game and this multiplies the capped rate." )
			.Default( 0 )
			.Keywords( "frame generation multiplier 2x 3x 4x 5x 6x 7x 8x double triple quadruple target fps dynamic interpolation off" );

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
			.DisabledUnless( TargetMode, "Frame generation is not set to Target fps" );

		a.Choice( "framegen.priority", "Priority",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.priority, kPriorityKeys, 2, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.priority = kPriorityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kPriorityOptions, std::size( kPriorityOptions ) )
			.Help( "Low latency adds the least delay, but motion can stutter when the game's "
			       "frame times jitter. Smoothness spaces the frames perfectly evenly and adds "
			       "about one game frame of delay." )
			.Default( 0 )
			.Keywords( "frame generation priority low latency smoothness input lag jitter pacing" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.quality", "Quality",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.quality, kQualityKeys, 2, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.quality = kQualityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kQualityOptions, std::size( kQualityOptions ) )
			.Help( "Performance estimates motion at a lower resolution: about a third cheaper on "
			       "the GPU, but it can miss thin, fast detail. Switching pauses the picture for "
			       "an instant." )
			.Default( 0 )
			.Keywords( "frame generation quality performance cost gpu motion estimation" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.safety", "Artifact safety",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.safety, kSafetyKeys, 3, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.safety = kSafetyKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kSafetyOptions, std::size( kSafetyOptions ) )
			.Help( "How readily a doubtful area falls back to the real frame. High shows fewer "
			       "smeared or warped spots but a little less smoothing; Low is smoother with "
			       "more visible mistakes." )
			.Default( 1 )
			.Keywords( "frame generation artifact safety artefacts ghosting trust fallback" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.hud_protection", "Static HUD protection",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.hud_protection, kHudKeys, 3, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.hud_protection = kHudKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kHudOptions, std::size( kHudOptions ) )
			.Help( "Keeps a motionless on-screen HUD from wobbling, and a still crosshair in the "
			       "middle of the screen sharp. The game's own HUD can still artifact when "
			       "frames are generated; this fork's own Crosshair is drawn after generation "
			       "and stays sharp." )
			.Default( 1 )
			.Keywords( "frame generation static hud protection ui wobble crosshair" )
			.DisabledUnless( On, "frame generation is off" );

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
			       "shows the presented rate; this fork's HUD shows the game rate, or the "
			       "presented rate with Count generated frames." )
			.Keywords( "frame generation status fps presented delay latency reason" );
	}
}
